#include "shell.h"

#ifdef __linux__

#include <sys/ptrace.h>

#include "syscall_table.h"

/* One summary row for a syscall. order is the index of its first call,
 * used to break ties when two rows have the same call count. */
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

/* Set when Ctrl-C arrives while attached with snoop -p. The handler is
 * installed without SA_RESTART, so the blocking waitpid it interrupts
 * returns with EINTR and the flag can be checked. */
static volatile sig_atomic_t snoop_interrupted = 0;

static void snoop_sigint(int sig) {
  (void)sig;
  snoop_interrupted = 1;
}

/* Returns the row for a syscall number, creating it on first sight.
 * Returns NULL only if memory could not be allocated. */
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
  r->order = t->n;
  t->n++;
  return r;
}

/* Looks up a syscall's name by number, falling back to a generated
 * "syscall_N" name if it is not in the table. */
static const char *syscall_name(long nr, char *buf, size_t size) {
  size_t count = sizeof(syscall_table) / sizeof(syscall_table[0]);
  for (size_t i = 0; i < count; i++)
    if (syscall_table[i].nr == nr)
      return syscall_table[i].name;
  snprintf(buf, size, "syscall_%ld", nr);
  return buf;
}

/* Orders rows by call count descending, breaking ties by the order the
 * syscall was first seen. */
static int cmp_stat(const void *a, const void *b) {
  const SyscallStat *x = a;
  const SyscallStat *y = b;
  if (x->calls != y->calls)
    return (x->calls < y->calls) ? 1 : -1;
  return x->order - y->order;
}

/* Prints the syscall summary table. The column widths match the spec,
 * and a name too long for its column, such as clock_nanosleep, simply
 * pushes the rest of its own row right while keeping a separating
 * space, so the header stays fixed and every row still splits cleanly
 * on whitespace. */
static void print_summary(StatTable *t) {
  qsort(t->rows, (size_t)t->n, sizeof(*t->rows), cmp_stat);

  char buf[32];
  printf("%-13s %-7s %s\n", "syscall", "calls", "time");
  for (int i = 0; i < t->n; i++) {
    const SyscallStat *r = &t->rows[i];
    printf("%-13s %-7ld %.3fs\n", syscall_name(r->nr, buf, sizeof(buf)),
           r->calls, (double)r->ns / 1e9);
  }
  fflush(stdout);
}

/* Returns the elapsed time in nanoseconds between two timestamps. */
static long long elapsed_ns(const struct timespec *from,
                            const struct timespec *to) {
  return (long long)(to->tv_sec - from->tv_sec) * 1000000000LL +
         (to->tv_nsec - from->tv_nsec);
}

/* Decides what signal, if any, should be handed back to the tracee at
 * a stop that is not a syscall stop. A real signal-delivery stop must
 * be passed on, or the tracee would never actually receive it, which
 * would mean Ctrl-C could not kill it. After PTRACE_ATTACH, though, a
 * group stop looks identical in the wait status, and re-injecting its
 * signal there would loop forever. PTRACE_GETSIGINFO tells the two
 * apart, since it fails with EINVAL only for a group stop. */
static int signal_to_inject(pid_t pid, int status) {
  int sig = WSTOPSIG(status);

  /* An exec by the tracee, reported because of PTRACE_O_TRACEEXEC, is
   * an event rather than a signal and needs nothing injected. */
  if (sig == SIGTRAP && (status >> 16) == PTRACE_EVENT_EXEC)
    return 0;

  siginfo_t si;
  if (ptrace(PTRACE_GETSIGINFO, pid, NULL, &si) < 0 && errno == EINVAL)
    return 0;
  return sig;
}

/* Stops tracing a live process and lets it continue running normally.
 * PTRACE_DETACH requires the tracee to be in a ptrace stop, so SIGSTOP
 * is sent and the loop runs until that exact signal is reported;
 * detaching from any earlier stop with signal 0 would discard it
 * instead of actually stopping the process. If the tracee exits before
 * that happens, its exit status is returned instead. */
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

/* Steps a traced process syscall by syscall until it exits, counting
 * each call and timing it.
 *
 * With PTRACE_O_TRACESYSGOOD set, a syscall stop is reported as
 * SIGTRAP with the high bit set, so it can never be confused with a
 * genuine SIGTRAP. Whether a given stop is a syscall entry or exit
 * comes from PTRACE_GET_SYSCALL_INFO rather than from toggling a flag
 * on each stop, since a flag would fall out of sync if a stop were
 * ever missed.
 *
 * A call is counted at its entry, because some calls such as
 * exit_group, or a successful execve, never produce a matching exit
 * even though the call did happen. Time is added at the matching exit,
 * so a call that never returns simply contributes zero time.
 *
 * attached enables the Ctrl-C detaching behaviour used by snoop -p. */
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

    /* The timestamp is taken before any other bookkeeping, so that
     * bookkeeping is never itself counted as part of the call. */
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
      /* Already sitting in a ptrace stop, so detaching can happen
       * right here, passing along any signal that was about to be
       * delivered. */
      ptrace(PTRACE_DETACH, pid, NULL, (void *)(long)inject);
      return TRACE_DETACHED;
    }
    if (ptrace(PTRACE_SYSCALL, pid, NULL, (void *)(long)inject) < 0 &&
        errno != ESRCH)
      return TRACE_FAILED;
  }
}

/* Implements "snoop command [args...]": runs a new command under
 * ptrace and prints a syscall summary once it exits. */
static void snoop_command(int argc, char **argv) {
  char *path = resolve_command(argv[1]);
  if (path == NULL) {
    fprintf(stderr, "snoop: command not found\n");
    return;
  }

  /* argv[1..] becomes the command's own NULL-terminated argument
   * vector. */
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
    /* The shell ignores or specially handles these signals, and an
     * ignored signal survives execve, so the command's own handling is
     * restored to the default before it runs. */
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

  /* A successful execve stops the child with SIGTRAP right before its
   * first instruction runs. If the child exited instead, the exec
   * itself must have failed. */
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  if (!WIFSTOPPED(status)) {
    fprintf(stderr, "snoop: command not found\n");
    return;
  }

  /* PTRACE_O_EXITKILL ensures the tracee is killed rather than left
   * running untraced if the shell itself dies. */
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

/* Implements "snoop -p pid": attaches to an already running process
 * and prints a syscall summary once it exits or is detached from. */
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

  /* A tracer receives every stop and the final exit of its tracee, so
   * if this pid is one of the shell's own background jobs, the
   * SIGCHLD handler must stop reaping it while it is being traced. It
   * is watched again below if it turns out to still be alive. */
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

  /* Attaching sends SIGSTOP, so that stop is waited for before the
   * tracer configures anything further. */
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  if (!WIFSTOPPED(status)) {
    /* The process exited in the meantime. If it was one of the
     * shell's own jobs, that is reported now. */
    bg_child_exited(pid, status);
    fprintf(stderr, "snoop: no such process\n");
    return;
  }

  /* EXITKILL is not set here, since this process was not the shell's
   * own to kill. */
  ptrace(PTRACE_SETOPTIONS, pid, NULL,
         (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC));

  struct sigaction sa, old_sa;
  sa.sa_handler = snoop_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  snoop_interrupted = 0;
  sigaction(SIGINT, &sa, &old_sa);

  StatTable t = {0};
  int result = trace_loop(pid, &t, 1, &status);

  sigaction(SIGINT, &old_sa, NULL);
  if (result == TRACE_FAILED)
    ptrace(PTRACE_DETACH, pid, NULL, NULL);

  print_summary(&t);
  free(t.rows);

  /* A tracer is the one to collect its tracee's exit, so if that
   * tracee was one of the shell's background jobs, check_bg_jobs would
   * never otherwise see it exit. Its status is handed over here so the
   * job does not stay listed forever. */
  if (result == TRACE_EXITED)
    bg_child_exited(pid, status);
  else if (was_watched)
    bg_watch_pid(pid);
}

/* Implements the snoop command, dispatching to either snoop_pid for
 * the "-p pid" form or snoop_command for running a new command. */
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

/* snoop relies on ptrace, which is Linux-specific, so on any other
 * platform it simply reports itself as unsupported. */
void snoop(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr, "snoop: not supported on this system\n");
}

#endif
