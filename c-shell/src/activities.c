#include "shell.h"

/* ------------------------------------------------------------------ */
/* procfs_available: is /proc mounted?  Probed once at runtime rather   */
/* than with #ifdef, so the same binary behaves correctly on Linux and  */
/* degrades gracefully on systems without procfs (e.g. macOS).          */
/* ------------------------------------------------------------------ */
static int procfs_available(void) {
  static int cached = -1;
  if (cached < 0)
    cached = (access("/proc/self/stat", F_OK) == 0);
  return cached;
}

/* ------------------------------------------------------------------ */
/* proc_state_char: read field 3 (state) of /proc/<pid>/stat.           */
/* Returns 0 if it cannot be read (no procfs, or the process is gone).  */
/*                                                                      */
/* Field 2 is the executable name in parentheses and the kernel does    */
/* NOT escape it, so it may contain both spaces and parentheses.  That  */
/* rules out "%*d %*s %c" (stops at a space inside the name) and        */
/* strchr(buf, ')') (stops at a ')' inside the name).  comm is the only */
/* field that can contain ')', so the LAST one always terminates it.    */
/* ------------------------------------------------------------------ */
static char proc_state_char(pid_t pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;

  /* A single read(): /proc files must be read in one go to get a
     coherent snapshot. */
  char buf[512];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return 0;
  buf[n] = '\0';

  char *end = strrchr(buf, ')');
  if (end == NULL)
    return 0;

  char *p = end + 1;
  while (*p == ' ')
    p++;
  return *p;
}

/* ------------------------------------------------------------------ */
/* activities — main entry point                                       */
/* ------------------------------------------------------------------ */
void activities(int argc, char **argv) {
  (void)argv;

  if (argc != 1) {
    fprintf(stderr, "activities: invalid syntax\n");
    return;
  }

  /* Spec: processes that have exited must be removed before printing.
     check_bg_jobs is the one place statuses reach the job table (both the
     SIGCHLD handler's queue and its own sweep) -- a separate reaper here
     would race it for statuses and lose completion messages. */
  check_bg_jobs();

  int njobs = bg_live_count();
  for (int i = 0; i < njobs; i++) {
    const BgJob *j = bg_job_at(i);
    if (j == NULL || j->nprocs == 0)
      continue;

    printf("[%d] pgid %d\n", j->job_number, (int)j->pgid);

    for (int k = 0; k < j->nprocs; k++) {
      pid_t pid = j->procs[k].pid;

      /* On Linux, a process with no /proc entry is provably gone even
         though it could not be reaped here (this happens when
         activities runs in a forked child, which sees a copy-on-write
         snapshot of the table).  Inert where procfs is absent. */
      char c = procfs_available() ? proc_state_char(pid) : 0;
      if (procfs_available() && c == 0)
        continue;

      /* Each process's own state: /proc when present ('T' stopped, 't'
         tracing stop; R/S/D/I are Running), otherwise the state the
         SIGCHLD handler last reported for it.  Never the job-wide flag,
         which says Stopped if any one process is. */
      int stopped = (c != 0) ? (c == 'T' || c == 't')
                             : j->procs[k].stopped;
      const char *state = stopped ? "Stopped" : "Running";

      /* Two-space indent, space-separated; no column alignment. */
      printf("  %d %s %s\n", (int)pid, j->procs[k].command_name, state);
    }
  }

  fflush(stdout);
}
