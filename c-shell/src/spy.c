#include "shell.h"

static pid_t shell_pid = 0;

void spy_init(void) {
  shell_pid = getpid();
}

/* ------------------------------------------------------------------ */
/* mode_type: lsof's TYPE column for a stat() mode.                     */
/* ------------------------------------------------------------------ */
static const char *mode_type(mode_t m) {
  if (S_ISREG(m))  return "REG";
  if (S_ISDIR(m))  return "DIR";
  if (S_ISCHR(m))  return "CHR";
  if (S_ISBLK(m))  return "BLK";
  if (S_ISFIFO(m)) return "FIFO";
  if (S_ISLNK(m))  return "LINK";
  if (S_ISSOCK(m)) return "SOCK";
  return "UNKNOWN";
}

/* ------------------------------------------------------------------ */
/* read_link: readlink() that NUL-terminates.  readlink never does, and */
/* silently truncates, so one byte is held back for the terminator.     */
/* ------------------------------------------------------------------ */
static int read_link(const char *link, char *buf, size_t size) {
  ssize_t n = readlink(link, buf, size - 1);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return 0;
}

/* Columns as in the spec: pid, 4 spaces, then FD and TYPE padded to 7. */
static void print_row(pid_t pid, const char *fd, const char *type,
                      const char *path) {
  printf("%d    %-6s %-6s %s\n", (int)pid, fd, type, path);
}

/* ------------------------------------------------------------------ */
/* show_link: one row for a /proc/<pid>/<name> magic link.              */
/*                                                                      */
/* PATH comes from readlink(); TYPE from stat() on the link itself.     */
/* The kernel resolves a magic link to the open object, not to its      */
/* name, so stat() still works for pipes, sockets and deleted files --  */
/* where "pipe:[123]" or "foo (deleted)" is not a path that exists.     */
/* Entries we may not read (another user's process) are skipped.        */
/* ------------------------------------------------------------------ */
static void show_link(pid_t pid, const char *name, const char *fd_label) {
  char link[64];
  char target[PATH_MAX];
  snprintf(link, sizeof(link), "/proc/%d/%s", (int)pid, name);
  if (read_link(link, target, sizeof(target)) < 0)
    return;

  const char *type = "UNKNOWN";
  struct stat st;
  if (strncmp(target, "anon_inode:", 11) == 0)
    type = "a_inode";               /* eventfd, epoll, ...: no file type */
  else if (stat(link, &st) == 0)
    type = mode_type(st.st_mode);

  print_row(pid, fd_label, type, target);
}

/* ------------------------------------------------------------------ */
/* show_mem: one row per unique file mapped into the process.           */
/*                                                                      */
/* /proc/<pid>/maps has one line per mapping:                           */
/*   address perms offset dev inode [pathname]                          */
/* A library appears several times (text, rodata, data segments), so    */
/* paths already printed are skipped.  Only real paths count: "[heap]", */
/* "[stack]", "[vdso]" and anonymous mappings have no file.  The        */
/* executable is also mapped, but is already reported as txt.           */
/* ------------------------------------------------------------------ */
static void show_mem(pid_t pid, const char *exe) {
  char maps[64];
  snprintf(maps, sizeof(maps), "/proc/%d/maps", (int)pid);
  FILE *fp = fopen(maps, "r");
  if (fp == NULL)
    return;

  char  **seen   = NULL;
  size_t  nseen  = 0;
  size_t  capseen = 0;
  char   *line   = NULL;
  size_t  linecap = 0;

  while (getline(&line, &linecap, fp) > 0) {
    /* Skip the five fixed fields; the path, if any, is the rest of the
       line and may itself contain spaces. */
    int off = -1;
    sscanf(line, "%*s %*s %*s %*s %*s %n", &off);
    if (off < 0)
      continue;
    char *file = line + off;
    file[strcspn(file, "\n")] = '\0';

    if (file[0] != '/')
      continue;
    if (exe != NULL && strcmp(file, exe) == 0)
      continue;

    int dup = 0;
    for (size_t i = 0; i < nseen && !dup; i++)
      dup = (strcmp(seen[i], file) == 0);
    if (dup)
      continue;

    if (nseen == capseen) {
      size_t ncap = capseen ? capseen * 2 : 16;
      char **grown = realloc(seen, ncap * sizeof(*seen));
      if (grown == NULL)
        break;
      seen = grown;
      capseen = ncap;
    }
    char *copy = strdup(file);
    if (copy == NULL)
      break;
    seen[nseen++] = copy;

    /* A file-backed mapping is a regular file unless stat() says
       otherwise (e.g. /dev/zero).  stat() fails for a mapped file that
       has since been deleted, which is still a regular file. */
    struct stat st;
    const char *type = (stat(file, &st) == 0) ? mode_type(st.st_mode) : "REG";
    print_row(pid, "mem", type, file);
  }

  for (size_t i = 0; i < nseen; i++)
    free(seen[i]);
  free(seen);
  free(line);
  fclose(fp);
}

static int cmp_int(const void *a, const void *b) {
  int x = *(const int *)a;
  int y = *(const int *)b;
  return (x > y) - (x < y);
}

/* ------------------------------------------------------------------ */
/* show_fds: one row per open descriptor, in ascending order.           */
/* readdir() returns /proc/<pid>/fd in no promised order, so the        */
/* numbers are collected and sorted first.                              */
/* ------------------------------------------------------------------ */
static void show_fds(pid_t pid) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/proc/%d/fd", (int)pid);
  DIR *d = opendir(dir);
  if (d == NULL)
    return;

  int    *fds = NULL;
  size_t  n   = 0;
  size_t  cap = 0;
  struct dirent *e;

  while ((e = readdir(d)) != NULL) {
    if (!isdigit((unsigned char)e->d_name[0]))
      continue;                     /* "." and ".." */
    char *end;
    long v = strtol(e->d_name, &end, 10);
    if (*end != '\0' || v < 0 || v > INT_MAX)
      continue;

    if (n == cap) {
      size_t ncap = cap ? cap * 2 : 16;
      int *grown = realloc(fds, ncap * sizeof(*fds));
      if (grown == NULL)
        break;
      fds = grown;
      cap = ncap;
    }
    fds[n++] = (int)v;
  }

  /* Close BEFORE reading the links.  When spying on ourselves this very
     directory stream is an open descriptor; once closed, its link is
     gone, readlink() fails, and spy does not report its own scaffolding. */
  closedir(d);

  qsort(fds, n, sizeof(*fds), cmp_int);
  for (size_t i = 0; i < n; i++) {
    char name[32];
    char label[16];
    snprintf(name, sizeof(name), "fd/%d", fds[i]);
    snprintf(label, sizeof(label), "%d", fds[i]);
    show_link(pid, name, label);
  }
  free(fds);
}

/* ------------------------------------------------------------------ */
/* is_zombie: has the process exited, leaving only its /proc entry      */
/* until the parent reaps it?  A zombie has released every open file,   */
/* so it no longer corresponds to a process spy can report on.          */
/*                                                                      */
/* State is the field after "(comm)" in /proc/<pid>/stat.  comm may     */
/* itself contain ')', so the LAST ')' ends it.                         */
/* ------------------------------------------------------------------ */
static int is_zombie(pid_t pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
  FILE *fp = fopen(path, "r");
  if (fp == NULL)
    return 0;
  char buf[512];
  size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  buf[n] = '\0';

  char *end = strrchr(buf, ')');
  if (end == NULL || end[1] != ' ')
    return 0;
  return end[2] == 'Z' || end[2] == 'X';
}

/* ------------------------------------------------------------------ */
/* spy — main entry point                                              */
/* ------------------------------------------------------------------ */
void spy(int argc, char **argv) {
  if (argc > 2) {
    fprintf(stderr, "spy: invalid syntax\n");
    return;
  }

  pid_t pid = (shell_pid > 0) ? shell_pid : getpid();
  int   too_big = 0;

  if (argc == 2) {
    const char *s = argv[1];
    if (*s == '\0') {
      fprintf(stderr, "spy: invalid syntax\n");
      return;
    }
    long value = 0;
    for (const char *p = s; *p != '\0'; p++) {
      if (!isdigit((unsigned char)*p)) {
        fprintf(stderr, "spy: invalid syntax\n");
        return;
      }
      if (!too_big) {
        value = value * 10 + (*p - '0');
        if (value > INT_MAX)
          too_big = 1;              /* a number, just not any pid */
      }
    }
    pid = too_big ? 0 : (pid_t)value;
  }

  if (access("/proc/self/maps", R_OK) != 0) {
    fprintf(stderr, "spy: /proc is not available on this system\n");
    return;
  }

  char procdir[64];
  struct stat st;
  snprintf(procdir, sizeof(procdir), "/proc/%d", (int)pid);
  if (pid <= 0 || stat(procdir, &st) != 0 || !S_ISDIR(st.st_mode) ||
      is_zombie(pid)) {
    fprintf(stderr, "spy: no such process\n");
    return;
  }

  printf("PID    FD    TYPE   PATH\n");

  char exe_link[64];
  char exe[PATH_MAX];
  snprintf(exe_link, sizeof(exe_link), "/proc/%d/exe", (int)pid);
  int have_exe = (read_link(exe_link, exe, sizeof(exe)) == 0);

  show_link(pid, "cwd", "cwd");
  show_link(pid, "exe", "txt");
  show_mem(pid, have_exe ? exe : NULL);
  show_fds(pid);

  fflush(stdout);
}
