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
/* proc_state_string: map the state character to what the spec prints.  */
/* 'T' (stopped) and 't' (tracing stop) are Stopped; R/S/D/Z/I are all  */
/* Running, and so is the unknown case -- a process still in the job    */
/* table has already survived check_bg_jobs(), so it is known live and  */
/* "we cannot prove it is stopped" correctly means Running.  This is    */
/* also the no-procfs fallback: one branch, no second code path.        */
/* ------------------------------------------------------------------ */
static const char *proc_state_string(pid_t pid) {
  char c = proc_state_char(pid);
  if (c == 'T' || c == 't')
    return "Stopped";
  return "Running";
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
      if (procfs_available() && proc_state_char(pid) == 0)
        continue;

      /* A job the shell itself suspended is known Stopped without
         asking procfs -- which is also the only way the state is right
         on systems with no /proc. */
      const char *state = j->stopped ? "Stopped" : proc_state_string(pid);

      /* Two-space indent, space-separated; no column alignment. */
      printf("  %d %s %s\n", (int)pid, j->procs[k].command_name, state);
    }
  }

  fflush(stdout);
}
