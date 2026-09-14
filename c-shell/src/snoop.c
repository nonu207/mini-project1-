#include "shell.h"

#ifdef __linux__

#include <sys/ptrace.h>

#include "syscall_table.h"

/* One summary row.  `order` is the index of the syscall's first call,
   which is the tie-break when two rows have the same count. */
typedef struct {
  long      nr;
  long      calls;
  long long ns;
  int       order;
} SyscallStat;

typedef struct {
  SyscallStat *rows;
  int          n;
  int          cap;
} StatTable;

/* How a trace ended. */
enum { TRACE_EXITED, TRACE_DETACHED, TRACE_FAILED };

/* Set by Ctrl-C while attached with -p.  The handler is installed without
   SA_RESTART so the blocking waitpid() returns EINTR and sees it. */
static volatile sig_atomic_t snoop_interrupted = 0;

static void snoop_sigint(int sig) {
  (void)sig;
  snoop_interrupted = 1;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

/* The row for `nr`, created on first sight.  NULL only if out of memory. */
static SyscallStat *stat_row(StatTable *t, long nr) {
  for (int i = 0; i < t->n; i++)
    if (t->rows[i].nr == nr)
      return &t->rows[i];

  if (t->n == t->cap) {
    int ncap = t->cap ? t->cap * 2 : 64;
    SyscallStat *grown = realloc(t->rows, (size_t)ncap * sizeof(*grown));
    if (grown == NULL)
      return NULL;
    t->rows = grown;
    t->cap  = ncap;
  }
  SyscallStat *r = &t->rows[t->n];
  r->nr    = nr;
  r->calls = 0;
  r->ns    = 0;
  r->order = t->n;                  /* rows are created in first-call order */
  t->n++;
  return r;
}

static const char *syscall_name(long nr, char *buf, size_t size) {
  size_t count = sizeof(syscall_table) / sizeof(syscall_table[0]);
  for (size_t i = 0; i < count; i++)
    if (syscall_table[i].nr == nr)
      return syscall_table[i].name;
  snprintf(buf, size, "syscall_%ld", nr);
  return buf;
}

/* Spec: by call count descending, ties by order of first occurrence. */
static int cmp_stat(const void *a, const void *b) {
  const SyscallStat *x = a;
  const SyscallStat *y = b;
  if (x->calls != y->calls)
    return (x->calls < y->calls) ? 1 : -1;
  return x->order - y->order;
}

static void print_summary(StatTable *t) {
  qsort(t->rows, (size_t)t->n, sizeof(*t->rows), cmp_stat);

  /* Columns exactly as in the spec: 14 and 8 wide.  A longer name (e.g.
     clock_nanosleep) pushes its own row right but keeps a separating
     space, so the header never changes and rows still split on blanks. */
  char buf[32];
  printf("%-13s %-7s %s\n", "syscall", "calls", "time");
  for (int i = 0; i < t->n; i++) {
    const SyscallStat *r = &t->rows[i];
    printf("%-13s %-7ld %.3fs\n", syscall_name(r->nr, buf, sizeof(buf)),
           r->calls, (double)r->ns / 1e9);
  }
  fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Tracing                                                             */
/* ------------------------------------------------------------------ */

static long long elapsed_ns(const struct timespec *from,
                            const struct timespec *to) {
  return (long long)(to->tv_sec - from->tv_sec) * 1000000000LL +
         (to->tv_nsec - from->tv_nsec);
}

/* ------------------------------------------------------------------ */
/* signal_to_inject: what to hand back to a tracee at a non-syscall stop. */
/*                                                                      */
/* A signal-delivery-stop must be passed on or the tracee never sees    */
/* the signal (so ^C would not kill it).  But after PTRACE_ATTACH a     */
/* group-stop looks the same in the wait status; re-injecting its      */
/* signal would loop forever.  PTRACE_GETSIGINFO tells them apart: it   */
/* fails with EINVAL only for a group-stop.                             */
/* ------------------------------------------------------------------ */
static int signal_to_inject(pid_t pid, int status) {
  int sig = WSTOPSIG(status);

  /* exec by the tracee (PTRACE_O_TRACEEXEC): an event, not a signal. */
  if (sig == SIGTRAP && (status >> 16) == PTRACE_EVENT_EXEC)
    return 0;

  siginfo_t si;
  if (ptrace(PTRACE_GETSIGINFO, pid, NULL, &si) < 0 && errno == EINVAL)
    return 0;
  return sig;
}

/* ------------------------------------------------------------------ */
/* detach: stop tracing a live process and leave it running.            */
/*                                                                      */
/* PTRACE_DETACH needs the tracee in a ptrace-stop, so send SIGSTOP and */
/* run until that very signal is reported; detaching from there with    */
/* signal 0 discards it, so the process never actually stops.  If it    */
/* exits first, *status receives the exit instead.                      */
/* ------------------------------------------------------------------ */
static int detach(pid_t pid, int *status) {
  kill(pid, SIGSTOP);
  for (;;) {
    if (waitpid(pid, status, 0) < 0) {
      if (errno == EINTR)
        continue;
      return TRACE_FAILED;
    }
    if (WIFEXITED(*status) || WIFSIGNALED(*status))
      return TRACE_EXITED;
    if (!WIFSTOPPED(*status))
      continue;
    if (WSTOPSIG(*status) == SIGSTOP) {
      ptrace(PTRACE_DETACH, pid, NULL, NULL);
      return TRACE_DETACHED;
    }
    int inject = (WSTOPSIG(*status) == (SIGTRAP | 0x80))
                     ? 0 : signal_to_inject(pid, *status);
    ptrace(PTRACE_CONT, pid, NULL, (void *)(long)inject);
  }
}

/* ------------------------------------------------------------------ */
/* trace_loop: step the tracee syscall by syscall until it exits.       */
/*                                                                      */
/* With PTRACE_O_TRACESYSGOOD a syscall-stop reports SIGTRAP|0x80, so   */
/* it can never be confused with a real SIGTRAP.  Whether a stop is an  */
/* entry or an exit comes from PTRACE_GET_SYSCALL_INFO rather than from  */
/* toggling a flag, which would desynchronise if a stop were missed.    */
/*                                                                      */
/* The call is counted at ENTRY: exit_group and a successful execve     */
/* never produce a matching exit, yet they did happen.  Time is added   */
/* at EXIT, so calls that never return contribute 0.000s.               */
/*                                                                      */
/* `attached` enables Ctrl-C detaching (for -p).                        */
/* ------------------------------------------------------------------ */
static int trace_loop(pid_t pid, StatTable *t, int attached, int *status) {
  long            pending_nr = -1;
  struct timespec entry_ts   = {0};

  if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) < 0)
    return TRACE_FAILED;

  for (;;) {
    if (waitpid(pid, status, 0) < 0) {
      if (errno != EINTR)
        return TRACE_FAILED;
      if (attached && snoop_interrupted)
        return detach(pid, status);
      continue;
    }

    /* Timestamp first, so bookkeeping below does not count as the call. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (WIFEXITED(*status) || WIFSIGNALED(*status))
      return TRACE_EXITED;
    if (!WIFSTOPPED(*status))
      continue;

    int inject = 0;
    if (WSTOPSIG(*status) == (SIGTRAP | 0x80)) {
      struct __ptrace_syscall_info info;
      long got = ptrace(PTRACE_GET_SYSCALL_INFO, pid,
                        (void *)sizeof(info), &info);
      if (got > 0 && info.op == PTRACE_SYSCALL_INFO_ENTRY) {
        pending_nr = (long)info.entry.nr;
        entry_ts   = now;
        SyscallStat *row = stat_row(t, pending_nr);
        if (row != NULL)
          row->calls++;
      } else if (got > 0 && info.op == PTRACE_SYSCALL_INFO_EXIT &&
                 pending_nr >= 0) {
        SyscallStat *row = stat_row(t, pending_nr);
        if (row != NULL)
          row->ns += elapsed_ns(&entry_ts, &now);
        pending_nr = -1;
      }
    } else {
      inject = signal_to_inject(pid, *status);
    }

    if (attached && snoop_interrupted) {
      /* Already in a ptrace-stop: detach right here, passing on any
         signal that was about to be delivered. */
      ptrace(PTRACE_DETACH, pid, NULL, (void *)(long)inject);
      return TRACE_DETACHED;
    }
    if (ptrace(PTRACE_SYSCALL, pid, NULL, (void *)(long)inject) < 0 &&
        errno != ESRCH)
      return TRACE_FAILED;
  }
}

/* ------------------------------------------------------------------ */
/* snoop command [args...]                                              */
/* ------------------------------------------------------------------ */
static void snoop_command(int argc, char **argv) {
  char *path = resolve_command(argv[1]);
  if (path == NULL) {
    fprintf(stderr, "snoop: command not found\n");
    return;
  }

  /* argv[1..] as the command's own NULL-terminated argument vector. */
  char **args = calloc((size_t)argc, sizeof(*args));
  if (args == NULL) {
    free(path);
    return;
  }
  for (int i = 1; i < argc; i++)
    args[i - 1] = argv[i];

  fflush(stdout);
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    free(args);
    free(path);
    return;
  }

  if (pid == 0) {
    /* The shell ignores or handles these; the command must not inherit
       that (ignored signals survive execve). */
    signal(SIGINT,  SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
      _exit(126);
    execv(path, args);
    _exit(127);
  }

  free(args);
  free(path);

  /* A successful execve stops the child with SIGTRAP before its first
     instruction.  An exit here means the exec itself failed. */
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  if (!WIFSTOPPED(status)) {
    fprintf(stderr, "snoop: command not found\n");
    return;
  }

  /* EXITKILL: if the shell dies, the tracee must not run on untraced. */
  ptrace(PTRACE_SETOPTIONS, pid, NULL,
         (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC |
                        PTRACE_O_EXITKILL));

  StatTable t = {0};
  int result = trace_loop(pid, &t, 0, &status);
  if (result == TRACE_FAILED) {
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
      ;
  }
  print_summary(&t);
  free(t.rows);
}

/* ------------------------------------------------------------------ */
/* snoop -p pid                                                        */
/* ------------------------------------------------------------------ */
static void snoop_pid(const char *arg) {
  long value = 0;
  for (const char *p = arg; *p != '\0'; p++) {
    if (!isdigit((unsigned char)*p)) {
      fprintf(stderr, "snoop: invalid syntax\n");
      return;
    }
    if (value <= INT_MAX)
      value = value * 10 + (*p - '0');
  }
  if (value <= 0 || value > INT_MAX) {
    fprintf(stderr, "snoop: no such process\n");
    return;
  }
  pid_t pid = (pid_t)value;

  /* A tracer collects every stop and the exit of its tracee, so if this
     is one of our background jobs the SIGCHLD handler must not reap it
     while we trace.  Re-watched below if it is still alive. */
  int was_watched = bg_unwatch_pid(pid);

  if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
    if (was_watched)
      bg_watch_pid(pid);
    if (errno == ESRCH)
      fprintf(stderr, "snoop: no such process\n");
    else if (errno == EPERM)
      fprintf(stderr, "snoop: permission denied\n");
    else
      perror("snoop");
    return;
  }

  /* Attaching sends SIGSTOP; wait for that stop before configuring. */
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  if (!WIFSTOPPED(status)) {
    /* It exited in the meantime.  If it was one of our jobs, say so. */
    bg_child_exited(pid, status);
    fprintf(stderr, "snoop: no such process\n");
    return;
  }

  /* No EXITKILL here: the process was not ours to kill. */
  ptrace(PTRACE_SETOPTIONS, pid, NULL,
         (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC));

  struct sigaction sa, old_sa;
  sa.sa_handler = snoop_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;                  /* no SA_RESTART: we need the EINTR */
  snoop_interrupted = 0;
  sigaction(SIGINT, &sa, &old_sa);

  StatTable t = {0};
  int result = trace_loop(pid, &t, 1, &status);

  sigaction(SIGINT, &old_sa, NULL);
  if (result == TRACE_FAILED)
    ptrace(PTRACE_DETACH, pid, NULL, NULL);

  print_summary(&t);
  free(t.rows);

  /* A tracer collects its tracee's exit.  When that tracee is one of the
     shell's background jobs, check_bg_jobs() will never see it now, so
     hand the status over or the job would stay listed forever. */
  if (result == TRACE_EXITED)
    bg_child_exited(pid, status);
  else if (was_watched)
    bg_watch_pid(pid);            /* detached and still running */
}

void snoop(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "snoop: invalid syntax\n");
    return;
  }
  if (strcmp(argv[1], "-p") == 0) {
    if (argc != 3) {
      fprintf(stderr, "snoop: invalid syntax\n");
      return;
    }
    snoop_pid(argv[2]);
    return;
  }
  snoop_command(argc, argv);
}

#else /* !__linux__ */

void snoop(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr, "snoop: not supported on this system\n");
}

#endif
