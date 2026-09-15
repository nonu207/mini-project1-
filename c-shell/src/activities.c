#include "shell.h"

/* Reports whether /proc is mounted on this system. This is checked once
 * at runtime and cached, rather than compiled in with #ifdef, so the
 * same binary works correctly on Linux and simply falls back to a less
 * precise process state on systems without procfs, such as macOS. */
static int procfs_available(void) {
  static int cached = -1;
  if (cached < 0)
    cached = (access("/proc/self/stat", F_OK) == 0);
  return cached;
}

/* Reads the state character, field 3, from /proc/<pid>/stat. Returns 0
 * if the file cannot be read, which happens when there is no procfs or
 * the process has already exited.
 *
 * The executable name in field 2 is written in parentheses and the
 * kernel does not escape it, so the name itself may contain spaces or
 * parentheses. That rules out scanning with a format such as
 * "%*d %*s %c", which would stop at a space inside the name, and it
 * rules out finding the first closing parenthesis, which would stop
 * inside the name too. The name is the only field that can contain a
 * closing parenthesis, so the last one in the line always marks the end
 * of the name field. */
static char proc_state_char(pid_t pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;

  /* The file is read in a single call so the snapshot is consistent. */
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

/* Implements the activities command. Lists every job the shell is
 * tracking, one line per job followed by one indented line per process
 * in it, showing that process's pid, command name and state.
 *
 * Exited processes are reaped through check_bg_jobs before printing, so
 * the listing only shows what is still alive. check_bg_jobs is also
 * where the SIGCHLD handler's queued results and its own cleanup sweep
 * both land, so calling it here rather than reaping separately avoids
 * two reapers racing for the same exit status and losing a completion
 * message.
 *
 * A process's state comes from /proc when it is available: T or t means
 * Stopped, anything else means Running. Where /proc is not available,
 * the state the SIGCHLD handler last recorded for that process is used
 * instead. Either way this looks at the individual process, never the
 * job's overall stopped flag, since a job can have some processes
 * stopped and others still running. On Linux a process with no /proc
 * entry at all is known to be gone even though it could not be reaped
 * from here, which happens when activities runs inside a forked child
 * and sees a stale copy of the job table; such a process is skipped. */
void activities(int argc, char **argv) {
  (void)argv;

  if (argc != 1) {
    fprintf(stderr, "activities: invalid syntax\n");
    return;
  }

  check_bg_jobs();

  int njobs = bg_live_count();
  for (int i = 0; i < njobs; i++) {
    const BgJob *j = bg_job_at(i);
    if (j == NULL || j->nprocs == 0)
      continue;

    printf("[%d] pgid %d\n", j->job_number, (int)j->pgid);

    for (int k = 0; k < j->nprocs; k++) {
      pid_t pid = j->procs[k].pid;

      char c = procfs_available() ? proc_state_char(pid) : 0;
      if (procfs_available() && c == 0)
        continue;

      int stopped = (c != 0) ? (c == 'T' || c == 't')
                             : j->procs[k].stopped;
      const char *state = stopped ? "Stopped" : "Running";

      printf("  %d %s %s\n", (int)pid, j->procs[k].command_name, state);
    }
  }

  fflush(stdout);
}
