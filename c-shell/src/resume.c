#include "shell.h"

/* ------------------------------------------------------------------ */
/* Timeout plumbing.                                                    */
/*                                                                      */
/* The handler only raises a flag: the wait below is a blocking         */
/* waitpid(), and SIGALRM without SA_RESTART makes it fail with EINTR,  */
/* which is what lets us notice the timeout at all.                     */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t alarm_fired = 0;

static void alarm_handler(int sig) {
  (void)sig;
  alarm_fired = 1;
}

/* ------------------------------------------------------------------ */
/* parse_job_number: accept "%3" or "3".  Returns 0 if not a positive   */
/* integer.                                                             */
/* ------------------------------------------------------------------ */
static int parse_job_number(const char *s) {
  if (s == NULL)
    return 0;
  if (*s == '%')
    s++;
  if (*s == '\0')
    return 0;

  int value = 0;
  for (const char *p = s; *p != '\0'; p++) {
    if (!isdigit((unsigned char)*p))
      return 0;
    value = value * 10 + (*p - '0');
    if (value > 1000000)
      return 0;
  }
  return value;                     /* 0 is rejected: jobs start at 1 */
}

/* ------------------------------------------------------------------ */
/* parse_seconds: strictly positive integer seconds for --timeout.      */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* resume_bg: mark Running and return to the prompt at once.            */
/* ------------------------------------------------------------------ */
static void resume_bg(const BgJob *job) {
  int    job_number = job->job_number;
  pid_t  pgid       = job->pgid;
  char   cmdline[BG_CMD_MAX];

  strncpy(cmdline, job->cmdline, sizeof(cmdline) - 1);
  cmdline[sizeof(cmdline) - 1] = '\0';

  bg_set_running(job_number);
  kill(-pgid, SIGCONT);

  /* Spec: "[job_number] + Running command" */
  printf("[%d] + Running %s\n", job_number, cmdline);
  fflush(stdout);
  /* Spec: do NOT wait, and do NOT touch the terminal. */
}

/* ------------------------------------------------------------------ */
/* resume_fg: hand the job the terminal and wait for it.                */
/*                                                                      */
/* timeout_secs of 0 means "no timer".                                  */
/* ------------------------------------------------------------------ */
static void resume_fg(const BgJob *job, int timeout_secs) {
  int   job_number = job->job_number;
  pid_t pgid       = job->pgid;
  int   n          = job->nprocs;
  pid_t pids[MAX_JOB_PROCS];
  char  cmdline[BG_CMD_MAX];

  /* Copy everything out first: the table may be mutated below, which
     would invalidate `job`. */
  for (int i = 0; i < n; i++)
    pids[i] = job->procs[i].pid;
  strncpy(cmdline, job->cmdline, sizeof(cmdline) - 1);
  cmdline[sizeof(cmdline) - 1] = '\0';

  /* We wait on these processes ourselves below, so the SIGCHLD handler
     must stop reaping them first.  bg_set_stopped re-watches any that
     stop again. */
  for (int i = 0; i < n; i++)
    bg_unwatch_pid(pids[i]);

  /* Spec: print the command line, as a normal foreground launch would. */
  printf("%s\n", cmdline);
  fflush(stdout);

  bg_set_running(job_number);

  /* Spec: give the job the terminal, then continue it.  Ordering
     matters -- a job that resumes and immediately reads the terminal
     would take SIGTTIN if it were not already the foreground group. */
  term_give(pgid);
  kill(-pgid, SIGCONT);

  /* Clear unconditionally, not just when arming a timer: a leftover 1
     from an earlier resume would otherwise be read as a timeout the
     next time waitpid is interrupted by an unrelated SIGCHLD. */
  alarm_fired = 0;

  struct sigaction sa, old_alarm;
  if (timeout_secs > 0) {
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                /* no SA_RESTART: we need the EINTR */
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
        break;
      }
      if (r < 0 && errno == EINTR) {
        /* Only a timer we actually armed can time this job out. */
        if (timeout_secs > 0 && alarm_fired) {
          timed_out = 1;
          break;
        }
        continue;                   /* a background SIGCHLD; keep waiting */
      }
      break;                        /* ECHILD: already reaped */
    }
  }

  if (timeout_secs > 0) {
    /* Spec: cancel the pending timer when the job settles first. */
    alarm(0);
    sigaction(SIGALRM, &old_alarm, NULL);
  }

  if (timed_out) {
    kill(-pgid, SIGTERM);
    /* Reap the terminated group so it leaves no zombies.  The job is
       gone from the table, so check_bg_jobs would not match these. */
    for (int i = 0; i < n; i++) {
      int status;
      if (pids[i] > 0)
        while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR)
          ;
    }
    term_take();
    fprintf(stderr, "resume: job timed out\n");
    /* Spec: a timed out job has been terminated, so drop it. */
    bg_remove_job(job_number);
    return;
  }

  term_take();

  /* Spec: only stopped jobs stay in the list. */
  if (sn > 0) {
    bg_set_stopped(job_number, stopped, sn);
    printf("[%d] + Stopped %s\n", job_number, cmdline);
    fflush(stdout);
  } else {
    bg_remove_job(job_number);
  }
}

/* ------------------------------------------------------------------ */
/* resume — main entry point                                           */
/* ------------------------------------------------------------------ */
void resume(int argc, char **argv) {
  /* resume %N fg | resume %N bg | resume %N fg --timeout S */
  if (argc < 3 || argc > 5) {
    fprintf(stderr, "resume: invalid syntax\n");
    return;
  }

  int job_number = parse_job_number(argv[1]);
  if (job_number <= 0) {
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
    /* --timeout is only meaningful with fg, and needs exactly one arg. */
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
