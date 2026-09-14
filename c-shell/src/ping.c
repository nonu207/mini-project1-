#include "shell.h"

/* ------------------------------------------------------------------ */
/* parse_signal: signal_number must be one or more decimal digits.      */
/* A leading '-' or '+' is invalid syntax, not a value to reduce.       */
/*                                                                      */
/* The modulo is folded in digit by digit, so an arbitrarily long       */
/* number never overflows: (v * 10 + d) % 64 == ((v % 64) * 10 + d) % 64.*/
/* Returns the reduced signal (0..63), or -1 on invalid syntax.         */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* parse_positive: a strictly positive decimal integer, or 0 if the     */
/* string is anything else.  Used for both pids and job numbers.        */
/* ------------------------------------------------------------------ */
static long parse_positive(const char *s) {
  if (s == NULL || *s == '\0')
    return 0;

  long value = 0;
  for (const char *p = s; *p != '\0'; p++) {
    if (!isdigit((unsigned char)*p))
      return 0;
    value = value * 10 + (*p - '0');
    if (value > 0x7fffffffL)
      return 0;                     /* larger than any pid or job number */
  }
  return value;
}

/* ------------------------------------------------------------------ */
/* find_job_by_pid: the tracked job that owns a live process `pid`.     */
/* ------------------------------------------------------------------ */
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

static int is_stop_signal(int sig) {
  return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

/* ------------------------------------------------------------------ */
/* ping — main entry point                                             */
/* ------------------------------------------------------------------ */
void ping(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "ping: invalid syntax\n");
    return;
  }

  /* Spec: validate signal_number BEFORE looking up the target. */
  int sig = parse_signal(argv[2]);
  if (sig < 0) {
    fprintf(stderr, "ping: invalid syntax\n");
    return;
  }

  /* Reap anything that has already exited, so a finished process is not
     mistaken for a tracked one (a zombie would still accept kill()). */
  check_bg_jobs();

  const char *target = argv[1];
  int by_job = (target[0] == '%');
  const BgJob *job = NULL;
  pid_t dest = 0;                   /* argument for kill() */

  if (by_job) {
    long num = parse_positive(target + 1);
    if (num > 0)
      job = bg_find_job((int)num);
    if (job != NULL && job->nprocs > 0)
      dest = -job->pgid;            /* negative pid == whole group */
  } else {
    long pid = parse_positive(target);
    if (pid > 0)
      job = find_job_by_pid((pid_t)pid);
    if (job != NULL)
      dest = (pid_t)pid;
  }

  /* Spec: only pids/jobs this shell spawned and still tracks count; any
     other pid, even one that exists on the system, is unknown. */
  if (dest == 0) {
    fprintf(stderr, "ping: no such process found\n");
    return;
  }

  if (kill(dest, sig) < 0 && errno == ESRCH) {
    fprintf(stderr, "ping: no such process found\n");
    return;
  }

  /* Keep the job table's notion of Stopped/Running in sync, so activities
     and the Ctrl-D check see the change even where /proc is unavailable.
     A single pid only speaks for its job when it is the whole job. */
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

  /* Spec: echo the signal number exactly as typed, not the reduced one. */
  printf("Sent signal %s to %s\n", argv[2], target);
  fflush(stdout);
}
