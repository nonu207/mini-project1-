#include "shell.h"

extern char **environ;

/* The background job table. MAX_BG_JOBS, MAX_JOB_PROCS and the BgJob
 * and BgProc shapes are defined in bg.h, since activities.c needs them
 * too.
 *
 * The array is kept dense: indices 0 through n_jobs are always the
 * live jobs, in launch order. That ordering is what lets activities
 * list jobs oldest first for free. An earlier scheme that scanned for
 * a free slot let the slot order drift away from launch order as soon
 * as any job finished. */
static BgJob bg_jobs[MAX_BG_JOBS];
static int   n_jobs          = 0;
static int   next_job_number = 0;

/* SIGCHLD reaping.
 *
 * The spec requires the SIGCHLD handler itself to reap terminated
 * background processes with waitpid and WNOHANG. It must never reap a
 * foreground child, since fg_wait, resume fg and snoop each wait on
 * those processes by pid themselves, so the handler only waits on the
 * pids in the watched array: the processes of tracked jobs that
 * nothing else is currently waiting on.
 *
 * printf is not safe to call from a signal handler, so the handler
 * only records each (pid, status) pair into a queue; check_bg_jobs
 * reports them later from ordinary program context, which is also the
 * only place the job table itself is touched.
 *
 * Stops and continues are queued the same way, with the pid staying
 * watched afterward, so a job's Stopped or Running state always
 * follows however the kernel actually signalled it.
 *
 * The handler is installed without SA_RESTART, since it also needs to
 * interrupt a blocking fgets in main with EINTR; that is what lets a
 * completion be reported while the shell is waiting for input. */
#define MAX_WATCH (MAX_BG_JOBS * MAX_JOB_PROCS)

static volatile pid_t        watched[MAX_WATCH];
static volatile sig_atomic_t watch_hi = 0;

static volatile pid_t        reaped_pid[MAX_WATCH];
static volatile int          reaped_status[MAX_WATCH];
static volatile sig_atomic_t reaped_n = 0;

static void sigchld_handler(int sig) {
  (void)sig;
  int saved_errno = errno;

  for (int i = 0; i < watch_hi && reaped_n < MAX_WATCH; i++) {
    pid_t pid = watched[i];
    if (pid <= 0)
      continue;
    int status;
    /* WNOHANG means this call never blocks. WUNTRACED and WCONTINUED
     * mean stops and continues are reported too; only an actual exit
     * ends the watch on this pid. */
    if (waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED) == pid) {
      if (!WIFSTOPPED(status) && !WIFCONTINUED(status))
        watched[i] = 0;
      reaped_pid[reaped_n] = pid;
      reaped_status[reaped_n] = status;
      reaped_n++;
    }
  }

  errno = saved_errno;
}

/* Blocks SIGCHLD so ordinary program context can safely edit the watch
 * list or drain the queue without the handler running in the middle. */
static void block_sigchld(sigset_t *old) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGCHLD);
  sigprocmask(SIG_BLOCK, &set, old);
}

static void restore_sigmask(const sigset_t *old) {
  sigprocmask(SIG_SETMASK, old, NULL);
}

/* Adds a pid to the set the SIGCHLD handler reaps. Does nothing if the
 * pid is already watched. */
void bg_watch_pid(pid_t pid) {
  if (pid <= 0)
    return;
  sigset_t old;
  block_sigchld(&old);

  int slot = -1;
  for (int i = 0; i < watch_hi; i++) {
    if (watched[i] == pid) {
      restore_sigmask(&old);
      return;
    }
    if (watched[i] == 0 && slot < 0)
      slot = i;
  }
  if (slot < 0 && watch_hi < MAX_WATCH)
    slot = watch_hi++;
  /* If the list is completely full the pid simply goes unwatched here;
   * check_bg_jobs's own sweep will still reap it eventually. */
  if (slot >= 0)
    watched[slot] = pid;

  restore_sigmask(&old);
}

/* Removes a pid from the set the SIGCHLD handler reaps, so that code
 * elsewhere can safely wait on it directly. Returns 1 if the pid was
 * being watched, 0 otherwise. */
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

/* Handles the shell being terminated by SIGHUP or SIGTERM.
 *
 * The spec requires that whenever the shell exits while jobs still
 * exist, whether through Ctrl-D or otherwise, every tracked job
 * receives SIGHUP and the shell does not wait for them. The job table
 * itself is not safe to read from a signal handler, but the watch list
 * is, and it holds every process of every tracked job, with the only
 * exception being a job that resume fg or snoop is currently waiting
 * on, which is in the foreground rather than being watched. SIGCONT is
 * sent as well, so a stopped process can actually act on the hangup.
 * Afterward the shell dies of the original signal, exactly as it would
 * have if no handler were installed at all. */
static pid_t shell_pid = 0;

static void hangup_handler(int sig) {
  /* A forked builtin or helper process inherits this handler until it
   * execs, and it must never hang up the shell's own jobs. */
  if (getpid() == shell_pid) {
    for (int i = 0; i < watch_hi; i++) {
      pid_t pid = watched[i];
      if (pid > 0) {
        kill(pid, SIGHUP);
        kill(pid, SIGCONT);
      }
    }
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

/* Initializes the background job subsystem: clears the job table and
 * the watch and reap queues, and installs the SIGCHLD handler along
 * with the SIGHUP and SIGTERM handlers that hang up tracked jobs on
 * exit. */
void init_bg(void) {
  memset(bg_jobs, 0, sizeof(bg_jobs));
  n_jobs = 0;
  next_job_number = 0;
  watch_hi = 0;
  reaped_n = 0;

  struct sigaction sa;
  sa.sa_handler = sigchld_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGCHLD, &sa, NULL);

  shell_pid = getpid();
  struct sigaction hs;
  hs.sa_handler = hangup_handler;
  sigemptyset(&hs.sa_mask);
  sigaddset(&hs.sa_mask, SIGCHLD);
  hs.sa_flags = 0;
  sigaction(SIGHUP, &hs, NULL);
  sigaction(SIGTERM, &hs, NULL);
}

/* Keeps a job's overall stopped flag in sync with its processes: a job
 * is considered stopped while any one of its processes is. */
static void sync_job_stopped(BgJob *j) {
  j->stopped = 0;
  for (int k = 0; k < j->nprocs; k++)
    if (j->procs[k].stopped)
      j->stopped = 1;
}

/* Appends a new job to the table and fills in its fields. Shared by
 * the background launch path and the Ctrl-Z stop path, which differ
 * only in what they print afterward. */
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
    n = MAX_JOB_PROCS;

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

  const char *cl = (cmdline != NULL && cmdline[0] != '\0') ? cmdline
                                                           : j->lead_name;
  strncpy(j->cmdline, cl, BG_CMD_MAX - 1);
  j->cmdline[BG_CMD_MAX - 1] = '\0';

  /* From this point on the SIGCHLD handler is responsible for reaping
   * these processes. */
  for (int i = 0; i < n; i++)
    bg_watch_pid(pids[i]);
  return j;
}

/* Registers a newly launched background job and prints its job number
 * and process id, as required, before any output the job itself
 * produces. For a pipeline the pid printed is the first stage's. */
void register_bg_group(pid_t pgid, const pid_t *pids,
                       char (*names)[BG_NAME_MAX], int n,
                       const char *cmdline) {
  BgJob *j = add_job(pgid, pids, names, n, cmdline);
  if (j == NULL)
    return;
  printf("[%d] %d\n", j->job_number, (int)j->lead_pid);
  fflush(stdout);
}

/* Registers a foreground job that has just been stopped by Ctrl-Z, and
 * prints the required stopped-job line. fg_wait passes only the
 * processes that actually stopped. */
void register_stopped_job(pid_t pgid, const pid_t *pids,
                          char (*names)[BG_NAME_MAX], int n,
                          const char *cmdline) {
  BgJob *j = add_job(pgid, pids, names, n, cmdline);
  if (j == NULL)
    return;
  for (int k = 0; k < j->nprocs; k++)
    j->procs[k].stopped = 1;
  sync_job_stopped(j);
  printf("[%d] + Stopped %s\n", j->job_number, j->cmdline);
  fflush(stdout);
}

/* Reports whether any tracked job is currently stopped. */
int bg_has_stopped(void) {
  for (int i = 0; i < n_jobs; i++)
    if (bg_jobs[i].stopped)
      return 1;
  return 0;
}

/* Sends SIGHUP to every tracked job's process group on shell exit, and
 * does not wait for any of them to terminate. A stopped process cannot
 * act on SIGHUP until it is running again, so SIGCONT is sent
 * afterward to wake it; otherwise the hangup would never actually take
 * effect and the job would outlive the shell. */
void bg_hangup_all(void) {
  for (int i = 0; i < n_jobs; i++) {
    if (bg_jobs[i].nprocs <= 0)
      continue;
    kill(-bg_jobs[i].pgid, SIGHUP);
    if (bg_jobs[i].stopped)
      kill(-bg_jobs[i].pgid, SIGCONT);
  }
}

/* Registers a single standalone background command. A standalone
 * command forms a process group of one, and setpgid already made its
 * pgid equal to its own pid. */
void register_bg_job(pid_t pid, const char *name, const char *cmdline) {
  pid_t pids[1] = { pid };
  char  names[1][BG_NAME_MAX];

  names[0][0] = '\0';
  if (name != NULL) {
    strncpy(names[0], name, BG_NAME_MAX - 1);
    names[0][BG_NAME_MAX - 1] = '\0';
  }
  register_bg_group(pid, pids, names, 1, cmdline);
}

/* Records that a tracked process has exited, however it was reaped,
 * removes it from its job, and if that was the job's last process,
 * announces the job's completion and drops it from the table. Returns
 * 1 if a completion message was printed, 0 otherwise. */
int bg_child_exited(pid_t pid, int status) {
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

    /* The lead process's own exit status identifies the whole job, and
     * it may exit long before the last stage of a pipeline does. */
    if (pid == j->lead_pid) {
      j->lead_status = status;
      j->lead_reaped = 1;
    }
    j->last_status = status;

    for (int k = found; k < j->nprocs - 1; k++)
      j->procs[k] = j->procs[k + 1];
    j->nprocs--;

    if (j->nprocs == 0) {
      int st = j->lead_reaped ? j->lead_status : j->last_status;
      /* Printed to stdout, the same stream as the prompt and the
       * job-number line, with no trailing period, matching the spec's
       * examples. The name and pid used are always the lead's, so
       * exactly one line is printed per job. */
      if (WIFEXITED(st)) {
        printf("%s with pid %d exited normally\n",
               j->lead_name, (int)j->lead_pid);
      } else if (WIFSIGNALED(st)) {
        printf("%s with pid %d exited abnormally\n",
               j->lead_name, (int)j->lead_pid);
      }
      fflush(stdout);

      for (int m = i; m < n_jobs - 1; m++)
        bg_jobs[m] = bg_jobs[m + 1];
      n_jobs--;
      return 1;
    }
    sync_job_stopped(j);
    return 0;
  }
  return 0;
}

/* Applies one wait status collected for a tracked process. A stop or a
 * continue updates that process's own state; anything else means the
 * process has exited. Returns 1 if a completion message was printed. */
static int record_status(pid_t pid, int status) {
  if (!WIFSTOPPED(status) && !WIFCONTINUED(status))
    return bg_child_exited(pid, status);

  for (int i = 0; i < n_jobs; i++) {
    BgJob *j = &bg_jobs[i];
    for (int k = 0; k < j->nprocs; k++) {
      if (j->procs[k].pid == pid) {
        j->procs[k].stopped = WIFSTOPPED(status) ? 1 : 0;
        sync_job_stopped(j);
        return 0;
      }
    }
  }
  return 0;
}

/* Drains everything the SIGCHLD handler has collected and reports it,
 * then sweeps up anything the handler was not watching. Returns how
 * many completion messages were printed. */
int check_bg_jobs(void) {
  int reported = 0;

  /* The queue is copied out with SIGCHLD blocked, so the handler
   * cannot append to it in the middle of the copy. */
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
    reported += record_status(pids[i], statuses[i]);

  /* This second sweep catches children the handler was not watching:
   * the feeder and tee helper processes used for background
   * redirections, and a job whose SIGCHLD arrived before its pid was
   * added to the watch list. It only runs from ordinary program
   * context with no foreground wait in progress, so it can never take
   * a foreground child. */
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
    reported += record_status(pid, status);
  return reported;
}

/* Returns the number of live tracked jobs, for activities to iterate
 * over. */
int bg_live_count(void) {
  return n_jobs;
}

/* Returns the job at a given index, in launch order, or NULL if the
 * index is out of range. */
const BgJob *bg_job_at(int idx) {
  if (idx < 0 || idx >= n_jobs)
    return NULL;
  return &bg_jobs[idx];
}

/* Finds a job by its job number, returning a mutable pointer for
 * internal use. */
static BgJob *find_mutable(int job_number) {
  for (int i = 0; i < n_jobs; i++)
    if (bg_jobs[i].job_number == job_number)
      return &bg_jobs[i];
  return NULL;
}

/* Finds a job by its job number, for lookups outside this file. */
const BgJob *bg_find_job(int job_number) {
  return find_mutable(job_number);
}

/* Marks every process in a job as running again. */
void bg_set_running(int job_number) {
  BgJob *j = find_mutable(job_number);
  if (j == NULL)
    return;
  for (int k = 0; k < j->nprocs; k++)
    j->procs[k].stopped = 0;
  sync_job_stopped(j);
}

/* Marks a job as stopped again, keeping only the processes in pids
 * as still part of it; the rest are assumed to have already exited.
 * Since resume fg stops watching a job's processes while it waits on
 * them directly, any that stop again are handed back to the SIGCHLD
 * handler here. */
void bg_set_stopped(int job_number, const pid_t *pids, int n) {
  BgJob *j = find_mutable(job_number);
  if (j == NULL)
    return;
  if (n > MAX_JOB_PROCS)
    n = MAX_JOB_PROCS;

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
  for (int i = 0; i < kn; i++) {
    j->procs[i] = kept[i];
    j->procs[i].stopped = 1;
  }
  j->nprocs = kn;
  sync_job_stopped(j);

  for (int i = 0; i < kn; i++)
    bg_watch_pid(j->procs[i].pid);
}

/* Removes a job from the table without printing anything, used when a
 * resumed foreground job finishes, or when a timed-out job has been
 * killed. */
void bg_remove_job(int job_number) {
  for (int i = 0; i < n_jobs; i++) {
    if (bg_jobs[i].job_number != job_number)
      continue;
    for (int k = 0; k < bg_jobs[i].nprocs; k++)
      bg_unwatch_pid(bg_jobs[i].procs[k].pid);
    for (int m = i; m < n_jobs - 1; m++)
      bg_jobs[m] = bg_jobs[m + 1];
    n_jobs--;
    return;
  }
}

/* Prepares a background child process, called right after setpgid.
 *
 * A background job must not have terminal input. On a real terminal,
 * process groups already enforce that on their own, since the job's
 * group is never the terminal's foreground group, so a read from the
 * terminal makes the kernel stop the job with SIGTTIN, which is the
 * behaviour shown by the spec's own "cat | sort &" example. The shell
 * itself ignores SIGTTIN and SIGTTOU, and an ignored signal survives
 * execve, so both are restored to their default action here; otherwise
 * the read would simply fail with an error and the job would exit
 * instead of stopping. Without a terminal at all, nothing else would
 * stop the job from reading the shell's own input, so its stdin is
 * replaced with /dev/null in that case. */
void bg_child_setup(void) {
  signal(SIGTTIN, SIG_DFL);
  signal(SIGTTOU, SIG_DFL);

  if (!term_is_tty()) {
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      close(devnull);
    }
  }
}

/* Builds a NULL-terminated argv array from a background command
 * group's tokens, stopping at the ampersand, and skipping redirection
 * operators and their targets. The caller must free the result. */
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

/* Runs one background command group, either a pipeline or a single
 * command, without waiting for it to finish. */
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

  /* Whether the command is a builtin is decided before trying to
   * resolve it as an executable, since a builtin has no file on disk
   * to find; resolving first would make "peek &", "reveal &",
   * "locate &" and "hop &" all incorrectly fail with command not
   * found. This matches the ordering used in execute_pipeline and
   * execute_pipeline_bg. */
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
    /* A background job must not be tied to the terminal. Terminal-
     * generated signals such as Ctrl-C and Ctrl-Z are sent to the
     * foreground process group, so this job is given a group of its
     * own; otherwise a later foreground command's Ctrl-C would kill it
     * too. */
    setpgid(0, 0);
    bg_child_setup();

    SavedFds saved;
    if (apply_redirections(start, &saved) < 0)
      _exit(1);
    /* This child never restores its own stdin and stdout, so the
     * backup copies are simply closed here; otherwise every background
     * program would inherit them as stray descriptors 3 and 4. */
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
        /* A fresh copy of the frecency database is loaded here, since
         * this child's own chdir calls do not affect the parent
         * shell's working directory anyway. */
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

  /* The process group is set from the parent side too, so whichever of
   * parent and child runs first, the group is set the same way, and
   * the job is never briefly left in the shell's own group. */
  setpgid(pid, pid);

  char cmdline[BG_CMD_MAX];
  token_group_to_string(start, cmdline, sizeof(cmdline));
  register_bg_job(pid, argv[0], cmdline);
  free(resolved);
  free(argv);
  return 0;
}
