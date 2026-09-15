#include "shell.h"

/* Parses a signal number argument, which must be one or more decimal
 * digits; a leading plus or minus sign makes it invalid syntax rather
 * than a value to reduce. The result is folded down to the range 0 to
 * 63 digit by digit as it is parsed, using the identity that
 * (v * 10 + d) % 64 equals ((v % 64) * 10 + d) % 64, so an arbitrarily
 * long string of digits never overflows. Returns the reduced signal
 * number, or -1 if the string is not valid syntax. */
static int parse_signal(const char *s) {
  if (s == NULL || *s == '\0')
    return -1;

  int reduced = 0;
  for (const char *p = s; *p != '\0'; p++) {
    if (!isdigit((unsigned char)*p))
      return -1;
    reduced = (reduced * 10 + (*p - '0')) % 64;
  }
  return reduced;
}

/* Parses a strictly positive decimal integer, used for both pids and
 * job numbers. Returns the value, or 0 if the string is empty, contains
 * anything other than digits, or is larger than any real pid or job
 * number could be. */
static long parse_positive(const char *s) {
  if (s == NULL || *s == '\0')
    return 0;

  long value = 0;
  for (const char *p = s; *p != '\0'; p++) {
    if (!isdigit((unsigned char)*p))
      return 0;
    value = value * 10 + (*p - '0');
    if (value > 0x7fffffffL)
      return 0;
  }
  return value;
}

/* Finds the tracked job that a live process with the given pid belongs
 * to, or NULL if no tracked job has that pid. */
static const BgJob *find_job_by_pid(pid_t pid) {
  int njobs = bg_live_count();
  for (int i = 0; i < njobs; i++) {
    const BgJob *j = bg_job_at(i);
    if (j == NULL)
      continue;
    for (int k = 0; k < j->nprocs; k++)
      if (j->procs[k].pid == pid)
        return j;
  }
  return NULL;
}

/* Reports whether a signal is one that stops a process. */
static int is_stop_signal(int sig) {
  return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

/* Implements the ping command, which sends a signal to a tracked pid or
 * job. The signal number is validated before the target is looked up,
 * so a bad signal number is always reported as invalid syntax even if
 * the target itself does not exist.
 *
 * A target starting with a percent sign names a job number, and the
 * signal is sent to that job's whole process group; otherwise the
 * target is a plain pid and the signal is sent to that process alone.
 * Only pids and jobs this shell itself spawned and is still tracking
 * are valid targets. Anything else, including a pid that exists on the
 * system but was not spawned by this shell, is reported as unknown.
 *
 * After a successful send, the job table's own idea of whether the job
 * is stopped or running is updated to match, so that activities and the
 * Ctrl-D check see the new state even on systems where the process
 * state cannot be read from /proc. A single pid only speaks for its
 * whole job when that job has exactly one process.
 *
 * On success the signal number is echoed back exactly as it was typed,
 * not the value it was reduced to modulo 64. */
void ping(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "ping: invalid syntax\n");
    return;
  }

  int sig = parse_signal(argv[2]);
  if (sig < 0) {
    fprintf(stderr, "ping: invalid syntax\n");
    return;
  }

  /* Anything that has already exited is reaped first, so a finished
   * process is never mistaken for a live tracked one; a zombie would
   * otherwise still accept a signal from kill(). */
  check_bg_jobs();

  const char *target = argv[1];
  int by_job = (target[0] == '%');
  const BgJob *job = NULL;
  pid_t dest = 0;

  if (by_job) {
    long num = parse_positive(target + 1);
    if (num > 0)
      job = bg_find_job((int)num);
    if (job != NULL && job->nprocs > 0)
      dest = -job->pgid;
  } else {
    long pid = parse_positive(target);
    if (pid > 0)
      job = find_job_by_pid((pid_t)pid);
    if (job != NULL)
      dest = (pid_t)pid;
  }

  if (dest == 0) {
    fprintf(stderr, "ping: no such process found\n");
    return;
  }

  if (kill(dest, sig) < 0 && errno == ESRCH) {
    fprintf(stderr, "ping: no such process found\n");
    return;
  }

  if (job->nprocs > 0 && (by_job || job->nprocs == 1)) {
    int job_number = job->job_number;
    if (sig == SIGCONT) {
      bg_set_running(job_number);
    } else if (is_stop_signal(sig)) {
      pid_t pids[MAX_JOB_PROCS];
      int n = job->nprocs;
      for (int i = 0; i < n; i++)
        pids[i] = job->procs[i].pid;
      bg_set_stopped(job_number, pids, n);
    }
  }

  printf("Sent signal %s to %s\n", argv[2], target);
  fflush(stdout);
}
