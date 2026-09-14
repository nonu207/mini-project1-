#include "shell.h"

extern char **environ;

/* ── Background job table ──────────────────────────────────────────── */
/* MAX_BG_JOBS / MAX_PROCS_PER_JOB and the BgJob/BgProc shapes live in   */
/* bg.h, because activities.c needs them too.                            */

/* Dense array: indices [0, n_jobs) are live, in launch order.  Keeping it
   dense is what makes "oldest first" free for activities -- with the old
   scan-for-a-free-slot scheme, slot order diverged from launch order as
   soon as any job finished. */
static BgJob bg_jobs[MAX_BG_JOBS];
static int   n_jobs          = 0;   /* live jobs */
static int   next_job_number = 0;   /* monotonic; never decremented */

/* ── SIGCHLD reaping ───────────────────────────────────────────────── */
/* Spec: the SIGCHLD handler itself reaps terminated background          */
/* processes with waitpid() and WNOHANG.  It must never reap a            */
/* foreground child -- fg_wait, resume fg and snoop wait on those by pid  */
/* -- so it only waits on the pids in `watched`: processes of tracked     */
/* jobs that nothing else is waiting on.  printf is not async-signal-     */
/* safe, so the handler only queues (pid, status); check_bg_jobs reports  */
/* them from main context.  The job table is still touched only there.    */
/*                                                                          */
/* No SA_RESTART: the signal also interrupts fgets in main with EINTR,     */
/* which is what lets a completion be reported while waiting for input.    */
#define MAX_WATCH (MAX_BG_JOBS * MAX_JOB_PROCS)

static volatile pid_t        watched[MAX_WATCH];  /* 0 marks a free slot */
static volatile sig_atomic_t watch_hi = 0;         /* in use: [0, watch_hi) */

static volatile pid_t        reaped_pid[MAX_WATCH];
static volatile int          reaped_status[MAX_WATCH];
static volatile sig_atomic_t reaped_n = 0;

static void sigchld_handler(int sig) {
  (void)sig;
  int saved_errno = errno;  /* don't clobber errno under main's feet */

  for (int i = 0; i < watch_hi && reaped_n < MAX_WATCH; i++) {
    pid_t pid = watched[i];
    if (pid <= 0)
      continue;
    int status;
    /* WNOHANG: never block.  No WUNTRACED: a stopped job stays put. */
    if (waitpid(pid, &status, WNOHANG) == pid) {
      watched[i] = 0;
      reaped_pid[reaped_n] = pid;
      reaped_status[reaped_n] = status;
      reaped_n++;
    }
  }

  errno = saved_errno;
}

/* Keep the handler out while main context edits the watch list or
   drains the queue. */
static void block_sigchld(sigset_t *old) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGCHLD);
  sigprocmask(SIG_BLOCK, &set, old);
}

static void restore_sigmask(const sigset_t *old) {
  sigprocmask(SIG_SETMASK, old, NULL);
}

void bg_watch_pid(pid_t pid) {
  if (pid <= 0)
    return;
  sigset_t old;
  block_sigchld(&old);

  int slot = -1;
  for (int i = 0; i < watch_hi; i++) {
    if (watched[i] == pid) {        /* already watched */
      restore_sigmask(&old);
      return;
    }
    if (watched[i] == 0 && slot < 0)
      slot = i;
  }
  if (slot < 0 && watch_hi < MAX_WATCH)
    slot = watch_hi++;
  /* A full list leaves the pid unwatched; check_bg_jobs's sweep still
     reaps it from main context. */
  if (slot >= 0)
    watched[slot] = pid;

  restore_sigmask(&old);
}

int bg_unwatch_pid(pid_t pid) {
  sigset_t old;
  block_sigchld(&old);

  int was_watched = 0;
  for (int i = 0; i < watch_hi; i++) {
    if (watched[i] == pid) {
      watched[i] = 0;
      was_watched = 1;
    }
  }
  while (watch_hi > 0 && watched[watch_hi - 1] == 0)
    watch_hi--;

  restore_sigmask(&old);
  return was_watched;
}

/* ── Public API ────────────────────────────────────────────────────── */

void init_bg(void) {
  memset(bg_jobs, 0, sizeof(bg_jobs));
  n_jobs = 0;
  next_job_number = 0;
  watch_hi = 0;
  reaped_n = 0;

  struct sigaction sa;
  sa.sa_handler = sigchld_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);
}

/* Append a job to the table and fill it in.  Shared by the background
   and the Ctrl-Z paths, which differ only in what they print. */
static BgJob *add_job(pid_t pgid, const pid_t *pids,
                      char (*names)[BG_NAME_MAX], int n,
                      const char *cmdline) {
  if (n <= 0)
    return NULL;
  if (n_jobs >= MAX_BG_JOBS) {
    fprintf(stderr, "cshell: too many background jobs\n");
    return NULL;
  }
  if (n > MAX_JOB_PROCS)
    n = MAX_JOB_PROCS;  /* extra stages still run, just untracked */

  BgJob *j = &bg_jobs[n_jobs++];
  memset(j, 0, sizeof(*j));
  j->job_number = ++next_job_number;
  j->pgid       = pgid;
  j->lead_pid   = pids[0];
  j->nprocs     = n;

  for (int i = 0; i < n; i++) {
    j->procs[i].pid = pids[i];
    strncpy(j->procs[i].command_name, names[i], BG_NAME_MAX - 1);
    j->procs[i].command_name[BG_NAME_MAX - 1] = '\0';
  }
  strncpy(j->lead_name, names[0], BG_NAME_MAX - 1);
  j->lead_name[BG_NAME_MAX - 1] = '\0';

  /* Fall back to the bare command name if no command line was captured. */
  const char *cl = (cmdline != NULL && cmdline[0] != '\0') ? cmdline
                                                           : j->lead_name;
  strncpy(j->cmdline, cl, BG_CMD_MAX - 1);
  j->cmdline[BG_CMD_MAX - 1] = '\0';

  /* From now on the SIGCHLD handler reaps these processes. */
  for (int i = 0; i < n; i++)
    bg_watch_pid(pids[i]);
  return j;
}

void register_bg_group(pid_t pgid, const pid_t *pids,
                       char (*names)[BG_NAME_MAX], int n,
                       const char *cmdline) {
  BgJob *j = add_job(pgid, pids, names, n, cmdline);
  if (j == NULL)
    return;
  /* Spec: print "[job_number] process_id" to stdout, before any output
     the command produces.  For a pipeline that pid is the first stage. */
  printf("[%d] %d\n", j->job_number, (int)j->lead_pid);
  fflush(stdout);
}

void register_stopped_job(pid_t pgid, const pid_t *pids,
                          char (*names)[BG_NAME_MAX], int n,
                          const char *cmdline) {
  BgJob *j = add_job(pgid, pids, names, n, cmdline);
  if (j == NULL)
    return;
  j->stopped = 1;
  /* Spec: "[job_number] + Stopped command" */
  printf("[%d] + Stopped %s\n", j->job_number, j->cmdline);
  fflush(stdout);
}

int bg_has_stopped(void) {
  for (int i = 0; i < n_jobs; i++)
    if (bg_jobs[i].stopped)
      return 1;
  return 0;
}

void bg_hangup_all(void) {
  for (int i = 0; i < n_jobs; i++) {
    if (bg_jobs[i].nprocs <= 0)
      continue;
    /* Negative pid == "the whole process group". */
    kill(-bg_jobs[i].pgid, SIGHUP);
    /* A stopped process cannot act on SIGHUP until it runs again, so
       wake it -- otherwise the hangup would never take effect and the
       job would outlive the shell. */
    if (bg_jobs[i].stopped)
      kill(-bg_jobs[i].pgid, SIGCONT);
  }
  /* Spec: do NOT wait for these processes to terminate. */
}

void register_bg_job(pid_t pid, const char *name, const char *cmdline) {
  pid_t pids[1] = { pid };
  char  names[1][BG_NAME_MAX];

  names[0][0] = '\0';
  if (name != NULL) {
    strncpy(names[0], name, BG_NAME_MAX - 1);
    names[0][BG_NAME_MAX - 1] = '\0';
  }
  /* A standalone command is a process group of one, and setpgid() made
     its pgid equal to its own pid. */
  register_bg_group(pid, pids, names, 1, cmdline);
}

int bg_child_exited(pid_t pid, int status) {
  /* The pid is gone, however it was reaped. */
  bg_unwatch_pid(pid);

  for (int i = 0; i < n_jobs; i++) {
    BgJob *j = &bg_jobs[i];

    int found = -1;
    for (int k = 0; k < j->nprocs; k++) {
      if (j->procs[k].pid == pid) {
        found = k;
        break;
      }
    }
    if (found < 0)
      continue;

    /* Remember the lead's verdict: it identifies the whole job, and it
       may exit long before the last stage does. */
    if (pid == j->lead_pid) {
      j->lead_status = status;
      j->lead_reaped = 1;
    }
    j->last_status = status;

    /* Drop just this process, keeping the rest in pipeline order. */
    for (int k = found; k < j->nprocs - 1; k++)
      j->procs[k] = j->procs[k + 1];
    j->nprocs--;

    /* The job retires only once every process is gone. */
    if (j->nprocs == 0) {
      int st = j->lead_reaped ? j->lead_status : j->last_status;
      /* Spec: print to stdout (same stream as the prompt and [N] pid).
         Format: "<name> with pid <pid> exited normally"   (WIFEXITED)
                 "<name> with pid <pid> exited abnormally" (WIFSIGNALED)
         Note: NO trailing period — the spec examples have none.
         The name and pid are the lead's, so one line per job. */
      if (WIFEXITED(st)) {
        printf("%s with pid %d exited normally\n",
               j->lead_name, (int)j->lead_pid);
      } else if (WIFSIGNALED(st)) {
        printf("%s with pid %d exited abnormally\n",
               j->lead_name, (int)j->lead_pid);
      }
      fflush(stdout);

      for (int m = i; m < n_jobs - 1; m++)  /* keep the array dense */
        bg_jobs[m] = bg_jobs[m + 1];
      n_jobs--;
      return 1;
    }
    return 0;  /* pids are unique across jobs */
  }
  return 0;
}

int check_bg_jobs(void) {
  int reported = 0;

  /* 1. Report what the SIGCHLD handler has reaped.  Copy the queue out
        with SIGCHLD blocked so the handler cannot append mid-copy. */
  static pid_t pids[MAX_WATCH];
  static int   statuses[MAX_WATCH];
  sigset_t old;
  block_sigchld(&old);
  int n = reaped_n;
  for (int i = 0; i < n; i++) {
    pids[i]     = reaped_pid[i];
    statuses[i] = reaped_status[i];
  }
  reaped_n = 0;
  restore_sigmask(&old);

  for (int i = 0; i < n; i++)
    reported += bg_child_exited(pids[i], statuses[i]);

  /* 2. Sweep up children the handler does not watch: the feeder/tee
        helpers of background redirections, and a job that exited before
        its pid was watched (its SIGCHLD came too early to match).  This
        runs only from main context with no foreground wait in progress,
        so it cannot take a foreground child. */
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    reported += bg_child_exited(pid, status);
  return reported;
}

/* ── Enumeration for activities (different translation unit) ────────── */

int bg_live_count(void) {
  return n_jobs;
}

const BgJob *bg_job_at(int idx) {
  if (idx < 0 || idx >= n_jobs)
    return NULL;
  return &bg_jobs[idx];
}

/* ── Lookup and mutation, for resume ──────────────────────────────── */

static BgJob *find_mutable(int job_number) {
  for (int i = 0; i < n_jobs; i++)
    if (bg_jobs[i].job_number == job_number)
      return &bg_jobs[i];
  return NULL;
}

const BgJob *bg_find_job(int job_number) {
  return find_mutable(job_number);
}

void bg_set_running(int job_number) {
  BgJob *j = find_mutable(job_number);
  if (j != NULL)
    j->stopped = 0;
}

void bg_set_stopped(int job_number, const pid_t *pids, int n) {
  BgJob *j = find_mutable(job_number);
  if (j == NULL)
    return;
  if (n > MAX_JOB_PROCS)
    n = MAX_JOB_PROCS;

  /* Keep the stored name for each surviving pid; the rest have exited. */
  BgProc kept[MAX_JOB_PROCS];
  int kn = 0;
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < j->nprocs; k++) {
      if (j->procs[k].pid == pids[i]) {
        kept[kn++] = j->procs[k];
        break;
      }
    }
  }
  for (int i = 0; i < kn; i++)
    j->procs[i] = kept[i];
  j->nprocs  = kn;
  j->stopped = 1;

  /* resume fg stops watching a job while it waits on it; hand the
     processes that stopped again back to the SIGCHLD handler. */
  for (int i = 0; i < kn; i++)
    bg_watch_pid(j->procs[i].pid);
}

void bg_remove_job(int job_number) {
  for (int i = 0; i < n_jobs; i++) {
    if (bg_jobs[i].job_number != job_number)
      continue;
    for (int k = 0; k < bg_jobs[i].nprocs; k++)
      bg_unwatch_pid(bg_jobs[i].procs[k].pid);
    for (int m = i; m < n_jobs - 1; m++)   /* keep the array dense */
      bg_jobs[m] = bg_jobs[m + 1];
    n_jobs--;
    return;
  }
}

/* ── Extract argv up to TOKEN_OP_AMP (for background commands).       */
/* Skips redirection operators (< > >>) and their targets. Caller      */
/* must free().                                                        */
/* ------------------------------------------------------------------- */
static char **extract_bg_argv(Token *start, int *out_argc) {
  int count = 0;
  int skip_next = 0;
  for (Token *t = start; t != NULL && t->type != TOKEN_OP_AMP; t = t->next) {
    if (t->type == TOKEN_OP_LT || t->type == TOKEN_OP_GT ||
        t->type == TOKEN_OP_GTGT) {
      skip_next = 1;
      continue;
    }
    if (skip_next) {
      skip_next = 0;
      continue;
    }
    if (t->type == TOKEN_WORD)
      count++;
  }

  char **argv = malloc((count + 1) * sizeof(char *));
  if (argv == NULL) {
    *out_argc = 0;
    return NULL;
  }

  int i = 0;
  skip_next = 0;
  for (Token *t = start; t != NULL && t->type != TOKEN_OP_AMP; t = t->next) {
    if (t->type == TOKEN_OP_LT || t->type == TOKEN_OP_GT ||
        t->type == TOKEN_OP_GTGT) {
      skip_next = 1;
      continue;
    }
    if (skip_next) {
      skip_next = 0;
      continue;
    }
    if (t->type == TOKEN_WORD)
      argv[i++] = t->value;
  }
  argv[i] = NULL;
  *out_argc = count;
  return argv;
}

int run_bg_group(Token *start, HopEntry *db, int *db_size) {
  (void)db;
  (void)db_size;

  if (start == NULL)
    return 0;

  int has_pipe = 0;
  for (Token *t = start; t != NULL && t->type != TOKEN_OP_AMP; t = t->next) {
    if (t->type == TOKEN_OP_PIPE) {
      has_pipe = 1;
      break;
    }
  }

  if (has_pipe) {
    pid_t pids[MAX_JOB_PROCS];
    char  names[MAX_JOB_PROCS][BG_NAME_MAX];
    int   n = 0;
    char cmdline[BG_CMD_MAX];
    token_group_to_string(start, cmdline, sizeof(cmdline));
    pid_t pgid = execute_pipeline_bg(start, pids, names, MAX_JOB_PROCS, &n);
    if (pgid > 0 && n > 0)
      register_bg_group(pgid, pids, names, n, cmdline);
    return 0;
  }

  int argc = 0;
  char **argv = extract_bg_argv(start, &argc);
  if (argv == NULL || argc == 0) {
    free(argv);
    return 0;
  }

  /* Decide builtin-ness BEFORE resolving a path: a builtin has no
     executable on disk, so resolving first made "peek &", "reveal &",
     "locate &" and "hop &" all fail with "command not found".  This
     ordering matches execute_pipeline / execute_pipeline_bg. */
  int is_builtin = (strcmp(argv[0], "peek") == 0 ||
                    strcmp(argv[0], "reveal") == 0 ||
                    strcmp(argv[0], "locate") == 0 ||
                    strcmp(argv[0], "hop") == 0 ||
                    strcmp(argv[0], "activities") == 0 ||
                    strcmp(argv[0], "ping") == 0 ||
                    strcmp(argv[0], "spy") == 0 ||
                    strcmp(argv[0], "snoop") == 0 ||
                    strcmp(argv[0], "resume") == 0);

  char *resolved = NULL;
  if (!is_builtin) {
    resolved = resolve_command(argv[0]);
    if (resolved == NULL) {
      const char *display = (argv[0][0] == '%') ? argv[0] + 1 : argv[0];
      fprintf(stderr, "cshell: command not found (%s)\n", display);
      free(argv);
      return 0;
    }
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    free(resolved);
    free(argv);
    return 0;
  }

  if (pid == 0) {
    /* Spec #12: a background job must not be tied to the terminal.
       Terminal-generated signals (^C -> SIGINT, ^Z -> SIGTSTP) are sent
       to the FOREGROUND process group, so give this job a group of its
       own; otherwise ^C during a later foreground command kills it too. */
    setpgid(0, 0);

    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      close(devnull);
    }

    SavedFds saved;
    if (apply_redirections(start, &saved) < 0)
      _exit(1);
    /* The child never restores its stdin/stdout, so drop the backup
       copies; otherwise every background program inherits them as
       stray descriptors 3 and 4. */
    close(saved.saved_stdin);
    close(saved.saved_stdout);

    if (is_builtin) {
      if (strcmp(argv[0], "peek") == 0) {
        peek(argc, argv);
      } else if (strcmp(argv[0], "reveal") == 0) {
        reveal(argc, argv);
      } else if (strcmp(argv[0], "locate") == 0) {
        locate(argc, argv);
      } else if (strcmp(argv[0], "activities") == 0) {
        activities(argc, argv);
      } else if (strcmp(argv[0], "resume") == 0) {
        resume(argc, argv);
      } else if (strcmp(argv[0], "ping") == 0) {
        ping(argc, argv);
      } else if (strcmp(argv[0], "spy") == 0) {
        spy(argc, argv);
      } else if (strcmp(argv[0], "snoop") == 0) {
        snoop(argc, argv);
      } else if (strcmp(argv[0], "hop") == 0) {
        /* Load a fresh copy of the db; the child's chdir does not
           affect the parent shell's working directory anyway.      */
        HopEntry local_db[MAX_HOP_ENTRIES];
        int local_sz = 0;
        load_hop_db(local_db, &local_sz);
        hop(argc, argv, local_db, &local_sz);
      }
      fflush(stdout);
      _exit(0);
    }

    execve(resolved, argv, environ);
    perror("execve");
    _exit(1);
  }

  /* Set the group from the parent as well: whichever side runs first
     wins, so the job is never briefly in the shell's group. */
  setpgid(pid, pid);

  char cmdline[BG_CMD_MAX];
  token_group_to_string(start, cmdline, sizeof(cmdline));
  register_bg_job(pid, argv[0], cmdline);
  free(resolved);
  free(argv);
  return 0;
}