#include "shell.h"

/* Set by alarm_handler when the resume --timeout timer fires. The
 * handler only raises this flag; the blocking waitpid it interrupts,
 * combined with SIGALRM having no SA_RESTART, is what lets the timeout
 * be noticed at all. */
static volatile sig_atomic_t alarm_fired = 0;

static void alarm_handler(int sig) {
  (void)sig;
  alarm_fired = 1;
}

/* Parses a job number argument. The required syntax is a percent sign
 * followed by decimal digits, so anything else is invalid syntax and
 * returns -1. "%0" or an unreasonably large number parses successfully
 * but simply names no real job, which the caller reports as no such
 * job rather than a syntax error. */
static int parse_job_number(const char *s) {
  if (s == NULL || *s != '%')
    return -1;
  s++;
  if (*s == '\0')
    return -1;

  int value = 0;
  for (const char *p = s; *p != '\0'; p++) {
    if (!isdigit((unsigned char)*p))
      return -1;
    /* Once past any value a real job number could have, stop growing
     * the number so it cannot overflow. */
    if (value <= 1000000)
      value = value * 10 + (*p - '0');
  }
  return value;
}

/* Parses a strictly positive integer number of seconds for --timeout.
 * Returns 0 if the string is not a valid positive integer. */
static int parse_seconds(const char *s) {
  if (s == NULL || *s == '\0')
    return 0;
  int value = 0;
  for (const char *p = s; *p != '\0'; p++) {
    if (!isdigit((unsigned char)*p))
      return 0;
    value = value * 10 + (*p - '0');
    if (value > 100000)
      return 0;
  }
  return value;
}

/* Resumes a job in the background: marks it Running, sends SIGCONT to
 * its process group, prints the confirmation line, and returns to the
 * prompt at once without waiting on the job or touching the terminal. */
static void resume_bg(const BgJob *job) {
  int    job_number = job->job_number;
  pid_t  pgid       = job->pgid;
  char   cmdline[BG_CMD_MAX];

  strncpy(cmdline, job->cmdline, sizeof(cmdline) - 1);
  cmdline[sizeof(cmdline) - 1] = '\0';

  bg_set_running(job_number);
  kill(-pgid, SIGCONT);

  printf("[%d] + Running %s\n", job_number, cmdline);
  fflush(stdout);
}

/* Resumes a job in the foreground: hands it the terminal, continues it,
 * and waits for it to finish or stop again. A timeout_secs of 0 means
 * no timer is armed. */
static void resume_fg(const BgJob *job, int timeout_secs) {
  int   job_number = job->job_number;
  pid_t pgid       = job->pgid;
  int   n          = job->nprocs;
  pid_t pids[MAX_JOB_PROCS];
  char  cmdline[BG_CMD_MAX];

  /* Everything needed is copied out first, because the job table may be
   * mutated below in ways that would invalidate the job pointer. */
  for (int i = 0; i < n; i++)
    pids[i] = job->procs[i].pid;
  strncpy(cmdline, job->cmdline, sizeof(cmdline) - 1);
  cmdline[sizeof(cmdline) - 1] = '\0';

  /* This function waits on these processes itself below, so the
   * SIGCHLD handler must stop reaping them first. bg_set_stopped will
   * hand any that stop again back to the handler. */
  for (int i = 0; i < n; i++)
    bg_unwatch_pid(pids[i]);

  printf("%s\n", cmdline);
  fflush(stdout);

  bg_set_running(job_number);

  /* The terminal is handed over before the job is continued. Doing it
   * in the other order would mean the job could read the terminal
   * before it is the foreground group, which would earn it a SIGTTIN. */
  term_give(pgid);
  kill(-pgid, SIGCONT);

  /* Cleared unconditionally rather than only when a timer is armed, so
   * a flag left over from an earlier resume is never mistaken for a
   * timeout the next time waitpid is interrupted by an unrelated
   * SIGCHLD. */
  alarm_fired = 0;

  struct sigaction sa, old_alarm;
  if (timeout_secs > 0) {
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, &old_alarm);
    alarm((unsigned int)timeout_secs);
  }

  pid_t stopped[MAX_JOB_PROCS];
  int   sn         = 0;
  int   timed_out  = 0;

  for (int i = 0; i < n && !timed_out; i++) {
    if (pids[i] <= 0)
      continue;
    for (;;) {
      int status;
      pid_t r = waitpid(pids[i], &status, WUNTRACED);
      if (r > 0) {
        if (WIFSTOPPED(status))
          stopped[sn++] = pids[i];
        else
          pids[i] = 0;
        break;
      }
      if (r < 0 && errno == EINTR) {
        /* Only a timer actually armed here can time this job out. */
        if (timeout_secs > 0 && alarm_fired) {
          timed_out = 1;
          break;
        }
        continue;
      }
      pids[i] = 0;
      break;
    }
  }

  if (timeout_secs > 0) {
    alarm(0);
    sigaction(SIGALRM, &old_alarm, NULL);
  }

  if (timed_out) {
    kill(-pgid, SIGTERM);
    /* The group is not waited for here, because a job that ignores
     * SIGTERM would otherwise hang the shell forever. The terminal is
     * reclaimed and the timeout reported right away. */
    term_take();
    fprintf(stderr, "resume: job timed out\n");
    /* A timed out job has been terminated, so it is dropped from the
     * job table. Its processes still need to be reaped once they
     * actually exit, so they are handed back to the SIGCHLD handler;
     * with no job owning them any more they are reaped silently. */
    bg_remove_job(job_number);
    for (int i = 0; i < n; i++)
      bg_watch_pid(pids[i]);
    return;
  }

  term_take();

  if (sn > 0) {
    bg_set_stopped(job_number, stopped, sn);
    if (term_is_tty())
      printf("\n");
    printf("[%d] + Stopped %s\n", job_number, cmdline);
    fflush(stdout);
  } else {
    bg_remove_job(job_number);
  }
}

/* Implements the resume command. Accepts "resume %N fg", "resume %N
 * bg", and "resume %N fg --timeout S", validates the syntax and looks
 * up the job before dispatching to resume_fg or resume_bg. */
void resume(int argc, char **argv) {
  if (argc < 3 || argc > 5) {
    fprintf(stderr, "resume: invalid syntax\n");
    return;
  }

  int job_number = parse_job_number(argv[1]);
  if (job_number < 0) {
    fprintf(stderr, "resume: invalid syntax\n");
    return;
  }

  int is_fg;
  if (strcmp(argv[2], "fg") == 0)
    is_fg = 1;
  else if (strcmp(argv[2], "bg") == 0)
    is_fg = 0;
  else {
    fprintf(stderr, "resume: invalid syntax\n");
    return;
  }

  int timeout_secs = 0;
  if (argc > 3) {
    /* --timeout only makes sense with fg, and needs exactly one value. */
    if (!is_fg || argc != 5 || strcmp(argv[3], "--timeout") != 0) {
      fprintf(stderr, "resume: invalid syntax\n");
      return;
    }
    timeout_secs = parse_seconds(argv[4]);
    if (timeout_secs <= 0) {
      fprintf(stderr, "resume: invalid syntax\n");
      return;
    }
  }

  const BgJob *job = bg_find_job(job_number);
  if (job == NULL || job->nprocs <= 0) {
    fprintf(stderr, "resume: no such job\n");
    return;
  }

  if (is_fg)
    resume_fg(job, timeout_secs);
  else
    resume_bg(job);
}
