#include "shell.h"

static pid_t shell_pid = 0;

/* Records the shell's own pid at startup, before any fork, so a bare
 * spy with no argument has something to report on. */
void spy_init(void) {
  shell_pid = getpid();
}

/* Maps a stat() mode to the TYPE column lsof would print for it. */
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

/* Reads a symlink into buf and NUL-terminates it. readlink never
 * terminates its result and silently truncates it if the buffer is too
 * small, so one byte of the buffer is held back for the terminator. */
static int read_link(const char *link, char *buf, size_t size) {
  ssize_t n = readlink(link, buf, size - 1);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return 0;
}

/* Prints one row in the spy output format: the pid, four spaces, then
 * the FD and TYPE columns padded to a fixed width, followed by the
 * path. */
static void print_row(pid_t pid, const char *fd, const char *type,
                      const char *path) {
  printf("%d    %-6s %-6s %s\n", (int)pid, fd, type, path);
}

/* Prints one row for a /proc/<pid>/<name> magic link, such as cwd, exe,
 * or one of the numbered entries under fd. The path comes from reading
 * the link itself, and the type comes from stat() on the link, which
 * the kernel resolves to the underlying open object rather than to a
 * name on disk. That is what makes stat() still work for a pipe, a
 * socket, or a file that has since been deleted, where the link's
 * target text, such as "pipe:[123]" or "foo (deleted)", is not itself a
 * path that exists. A link belonging to a process this shell is not
 * allowed to read is simply skipped. */
static void show_link(pid_t pid, const char *name, const char *fd_label) {
  char link[64];
  char target[PATH_MAX];
  snprintf(link, sizeof(link), "/proc/%d/%s", (int)pid, name);
  if (read_link(link, target, sizeof(target)) < 0)
    return;

  const char *type = "UNKNOWN";
  struct stat st;
  if (strncmp(target, "anon_inode:", 11) == 0)
    type = "a_inode";
  else if (stat(link, &st) == 0)
    type = mode_type(st.st_mode);

  print_row(pid, fd_label, type, target);
}

/* Prints one row per unique file memory-mapped into a process.
 * /proc/<pid>/maps has one line per mapping, giving the address range,
 * permissions, offset, device, inode and, if the mapping is
 * file-backed, a pathname. A shared library typically appears several
 * times for its text, read-only data and writable data segments, so
 * paths already printed are skipped. Entries such as [heap], [stack],
 * [vdso] and anonymous mappings have no real path and are ignored. The
 * process's own executable is mapped too, but it has already been
 * reported separately as the txt entry, so it is skipped here if its
 * path is known. */
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
    /* The first five fields are skipped; whatever remains on the line
     * is the pathname, if there is one, and it may itself contain
     * spaces. */
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

    /* A file-backed mapping is treated as a regular file unless stat()
     * says otherwise, such as for /dev/zero. stat() can fail for a
     * mapped file that has since been deleted, which is still, in
     * effect, a regular file. */
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

/* Prints one row per open file descriptor of a process, in ascending
 * numeric order. readdir() returns the entries of /proc/<pid>/fd in no
 * particular order, so the descriptor numbers are collected first and
 * sorted before anything is printed. */
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
      continue;
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

  /* The directory is closed before its entries' links are read. When
   * spying on the shell's own process, this very directory stream is
   * itself an open descriptor, and once it is closed its link
   * disappears, so readlink() on it fails and spy does not report its
   * own scaffolding as an open file. */
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

/* Reports whether a process has already exited and is only present as
 * a zombie waiting for its parent to reap it. A zombie has released
 * every open file, so it no longer corresponds to anything spy can
 * usefully report on.
 *
 * The state character is the field right after the parenthesized
 * command name in /proc/<pid>/stat. Since that name may itself contain
 * a closing parenthesis, the last one in the line is used to find the
 * end of the name field. */
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

/* Implements the spy command. With no argument it reports on the shell
 * itself; with one argument it reports on that pid. Prints the working
 * directory, executable, memory-mapped files and open descriptors of
 * the target process, in the same style as lsof. Requires /proc to be
 * available and the target to be a real, non-zombie process. */
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
          too_big = 1;
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
