#include "shell.h"

extern char **environ;

/* ------------------------------------------------------------------ */
/* child_default_signals: a job must react to the keyboard normally.   */
/* The shell ignores/handles these; children must not inherit that.    */
/* ------------------------------------------------------------------ */
static void child_default_signals(void) {
  signal(SIGINT,  SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGTTOU, SIG_DFL);
  signal(SIGTTIN, SIG_DFL);
  /* The shell ignores SIGPIPE while wiring a pipeline, and an ignored
     signal survives execve.  Restore it, or a stage whose reader has gone
     ("yes | head -1") prints "Broken pipe" instead of exiting quietly. */
  signal(SIGPIPE, SIG_DFL);
}

/* ------------------------------------------------------------------ */
/* fg_wait: wait for an entire foreground job, then take the terminal  */
/* back.  WUNTRACED is what makes Ctrl-Z observable -- without it a    */
/* stopped child never causes waitpid to return and the shell hangs.   */
/*                                                                      */
/* If the group was stopped it is handed to the job table so it gets a */
/* job number, shows up in activities, and blocks Ctrl-D.  Only the    */
/* processes that actually stopped are recorded: a stage that had      */
/* already exited must not be resurrected as a stopped process.        */
/* Returns 1 if the job stopped, 0 if every process finished.          */
/* ------------------------------------------------------------------ */
static int fg_wait(pid_t pgid, pid_t *pids, char (*names)[BG_NAME_MAX],
                   int n, const char *cmdline) {
  int sn = 0;
  int interrupted = 0;

  for (int i = 0; i < n; i++) {
    if (pids[i] <= 0)
      continue;
    int status;
    while (waitpid(pids[i], &status, WUNTRACED) < 0 && errno == EINTR)
      ;
    if (WIFSTOPPED(status)) {
      /* Compact the survivors to the front, keeping pipeline order. */
      pids[sn] = pids[i];
      if (sn != i)
        memcpy(names[sn], names[i], BG_NAME_MAX);
      sn++;
    } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) {
      interrupted = 1;
    }
  }

  /* Spec: reclaim the terminal after the pipeline finishes OR stops. */
  term_take();

  /* The job owned the terminal, so ^C/^Z went to it and never to the
     shell -- the shell must therefore end the echoed "^C" or "^Z" line
     itself, or the next output runs on from it ("^Z[1] + Stopped ...").
     A stop is only echoed on a terminal. */
  if (interrupted || (sn > 0 && term_is_tty())) {
    printf("\n");
    fflush(stdout);
  }

  if (sn > 0) {
    register_stopped_job(pgid, pids, names, sn, cmdline);
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* check_executable: returns 1 if path is an executable regular file. */
/* ------------------------------------------------------------------ */
static int check_executable(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)    return 0;
  if (S_ISDIR(st.st_mode))     return 0;
  if (access(path, X_OK) != 0) return 0;
  return 1;
}

/* ------------------------------------------------------------------ */
/* resolve_command: resolve a command name to an executable path.      */
/* Returns a heap-allocated string (caller must free), or NULL.        */
/* ------------------------------------------------------------------ */
char *resolve_command(const char *name) {
  if (name == NULL || name[0] == '\0')
    return NULL;

  char fullpath[PATH_MAX];
  int name_has_slash = (strchr(name, '/') != NULL);
  int percent_skip = (name[0] == '%');
  const char *lookup = percent_skip ? name + 1 : name;

  /* ── 1. Literal path (contains /) ────────────────────────────────── */
  if (name_has_slash) {
    if (check_executable(name))
      return strdup(name);
    return NULL;
  }

  /* ── 2. Check CWD first (unless % prefix) ────────────────────────── */
  if (!percent_skip) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      /* Skip, rather than probe, a path truncated to fit PATH_MAX. */
      int len = snprintf(fullpath, sizeof(fullpath), "%s/%s", cwd, lookup);
      if (len >= 0 && (size_t)len < sizeof(fullpath) &&
          check_executable(fullpath))
        return strdup(fullpath);
    }
  }

  /* ── 3. Search PATH ───────────────────────────────────────────────── */
  const char *path_env = getenv("PATH");
  if (path_env == NULL)
    return NULL;

  char *path_copy = strdup(path_env);
  if (path_copy == NULL)
    return NULL;

  char *dir = strtok(path_copy, ":");
  while (dir != NULL) {
    snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, lookup);
    if (check_executable(fullpath)) {
      char *result = strdup(fullpath);
      free(path_copy);
      return result;
    }
    dir = strtok(NULL, ":");
  }

  free(path_copy);
  return NULL;
}

/* ================================================================== */
/* Redirection support                                                 */
/*                                                                      */
/* One < or > is a file opened in the shell and dup2'd onto the        */
/* command's stdin/stdout.  Several < files are joined into one stream */
/* by a small feeder process writing them, in order, into a pipe;      */
/* several > / >> files are filled by a small tee process copying a    */
/* pipe into each of them.  The helpers run alongside the command, so  */
/* the shell never pumps data itself and cannot deadlock, and output   */
/* reaches the files as it is produced.                                */
/* ================================================================== */

/* is_redir_end: does t end the scan of a command's tokens [start, end)? */
static int is_redir_end(Token *t, Token *end) {
  return t == NULL || t == end ||
         t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP;
}

/* output_flags: open() flags for a > (truncate) or >> (append) target. */
static int output_flags(TokenType op) {
  return O_WRONLY | O_CREAT | (op == TOKEN_OP_GTGT ? O_APPEND : O_TRUNC);
}

/* close_fds_from: close every descriptor >= low.  A helper must hold  */
/* no pipe end but its own, or a reader elsewhere would never see EOF. */
static void close_fds_from(int low) {
  long max = sysconf(_SC_OPEN_MAX);
  if (max < 0 || max > 4096)
    max = 4096;
  for (int fd = low; fd < max; fd++)
    close(fd);
}

/* write_all: write the whole buffer, retrying short writes. */
static int write_all(int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t w = write(fd, buf, len);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    buf += w;
    len -= (size_t)w;
  }
  return 0;
}

int redirs_validate(Token *start, Token *end) {
  for (Token *t = start; !is_redir_end(t, end); t = t->next) {
    if (t->type == TOKEN_OP_LT) {
      t = t->next;
      if (is_redir_end(t, end) || t->type != TOKEN_WORD)
        return 0;
      int fd = open(t->value, O_RDONLY);
      if (fd < 0) {
        fprintf(stderr, "cshell: no such file or directory\n");
        return 0;
      }
      close(fd);
    } else if (t->type == TOKEN_OP_GT || t->type == TOKEN_OP_GTGT) {
      TokenType op = t->type;
      t = t->next;
      if (is_redir_end(t, end) || t->type != TOKEN_WORD)
        return 0;
      int fd = open(t->value, output_flags(op), 0644);
      if (fd < 0) {
        fprintf(stderr, "cshell: unable to create file for writing\n");
        return 0;
      }
      close(fd);
    }
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* spawn_feeder: fork a process that writes every < file of            */
/* [start, end), in order, into a pipe.  Returns the pipe's read end   */
/* (to become the command's stdin) and sets *pid_out, or returns -1.   */
/* ------------------------------------------------------------------ */
static int spawn_feeder(Token *start, Token *end, pid_t *pid_out) {
  int p[2];
  if (pipe(p) < 0) {
    perror("pipe");
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(p[0]);
    close(p[1]);
    return -1;
  }

  if (pid == 0) {
    signal(SIGPIPE, SIG_DFL);   /* command stopped reading: just stop */
    dup2(p[1], STDOUT_FILENO);
    close_fds_from(3);

    char buf[4096];
    for (Token *t = start; !is_redir_end(t, end); t = t->next) {
      if (t->type != TOKEN_OP_LT)
        continue;
      t = t->next;
      if (is_redir_end(t, end))
        break;
      int fd = open(t->value, O_RDONLY);
      if (fd < 0)
        continue;
      ssize_t n;
      while ((n = read(fd, buf, sizeof(buf))) != 0) {
        if (n < 0) {
          if (errno == EINTR)
            continue;
          break;
        }
        if (write_all(STDOUT_FILENO, buf, (size_t)n) < 0)
          _exit(1);
      }
      close(fd);
    }
    _exit(0);
  }

  close(p[1]);
  *pid_out = pid;
  return p[0];
}

/* ------------------------------------------------------------------ */
/* spawn_tee: fork a process that copies a pipe into every > / >> file */
/* of [start, end), each opened with its own mode.  Returns the pipe's */
/* write end (to become the command's stdout) and sets *pid_out, or    */
/* returns -1.                                                          */
/* ------------------------------------------------------------------ */
static int spawn_tee(Token *start, Token *end, int n_out, pid_t *pid_out) {
  int p[2];
  if (pipe(p) < 0) {
    perror("pipe");
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(p[0]);
    close(p[1]);
    return -1;
  }

  if (pid == 0) {
    signal(SIGPIPE, SIG_DFL);
    dup2(p[0], STDIN_FILENO);
    close_fds_from(3);

    int *fds = malloc((size_t)n_out * sizeof(int));
    if (fds == NULL)
      _exit(1);
    int k = 0;
    for (Token *t = start; !is_redir_end(t, end) && k < n_out; t = t->next) {
      if (t->type != TOKEN_OP_GT && t->type != TOKEN_OP_GTGT)
        continue;
      TokenType op = t->type;
      t = t->next;
      if (is_redir_end(t, end))
        break;
      fds[k++] = open(t->value, output_flags(op), 0644);
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) != 0) {
      if (n < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      /* A file that fails a write is dropped; the others keep going. */
      for (int i = 0; i < k; i++) {
        if (fds[i] >= 0 && write_all(fds[i], buf, (size_t)n) < 0) {
          close(fds[i]);
          fds[i] = -1;
        }
      }
    }
    _exit(0);
  }

  close(p[0]);
  *pid_out = pid;
  return p[1];
}

int redirs_open(Token *start, Token *end, Redirs *r) {
  r->in_fd = -1;
  r->out_fd = -1;
  r->helper[0] = 0;
  r->helper[1] = 0;

  /* Count the targets, remembering the last of each kind for the
     single-file case. */
  int n_in = 0, n_out = 0;
  Token *last_in = NULL;   /* the < target word  */
  Token *last_out = NULL;  /* the > or >> operator */
  for (Token *t = start; !is_redir_end(t, end); t = t->next) {
    if (t->type == TOKEN_OP_LT && t->next != NULL) {
      n_in++;
      last_in = t->next;
      t = t->next;
    } else if ((t->type == TOKEN_OP_GT || t->type == TOKEN_OP_GTGT) &&
               t->next != NULL) {
      n_out++;
      last_out = t;
      t = t->next;
    }
  }

  if (n_in == 1) {
    r->in_fd = open(last_in->value, O_RDONLY);
    if (r->in_fd < 0) {
      fprintf(stderr, "cshell: no such file or directory\n");
      return -1;
    }
  } else if (n_in > 1) {
    r->in_fd = spawn_feeder(start, end, &r->helper[0]);
    if (r->in_fd < 0)
      return -1;
  }

  if (n_out == 1) {
    r->out_fd = open(last_out->next->value, output_flags(last_out->type),
                     0644);
    if (r->out_fd < 0)
      fprintf(stderr, "cshell: unable to create file for writing\n");
  } else if (n_out > 1) {
    r->out_fd = spawn_tee(start, end, n_out, &r->helper[1]);
  }

  if (n_out > 0 && r->out_fd < 0) {
    redirs_close(r);   /* feeder sees its reader gone and exits */
    redirs_wait(r);
    r->helper[0] = 0;
    r->helper[1] = 0;
    return -1;
  }
  return 0;
}

void redirs_install(Redirs *r) {
  if (r->in_fd >= 0) {
    dup2(r->in_fd, STDIN_FILENO);
    close(r->in_fd);
    r->in_fd = -1;
  }
  if (r->out_fd >= 0) {
    dup2(r->out_fd, STDOUT_FILENO);
    close(r->out_fd);
    r->out_fd = -1;
  }
}

void redirs_close(Redirs *r) {
  if (r->in_fd >= 0) {
    close(r->in_fd);
    r->in_fd = -1;
  }
  if (r->out_fd >= 0) {
    close(r->out_fd);
    r->out_fd = -1;
  }
}

void redirs_wait(const Redirs *r) {
  for (int i = 0; i < 2; i++) {
    if (r->helper[i] <= 0)
      continue;
    while (waitpid(r->helper[i], NULL, 0) < 0 && errno == EINTR)
      ;
  }
}

/* ------------------------------------------------------------------ */
/* execute_command: fork + execve an external command.                 */
/* Returns 0 on success, 1 if the command was not found.               */
/* ------------------------------------------------------------------ */
int execute_command(int argc, char **argv, Token *start) {
  if (argc < 1 || argv[0] == NULL)
    return 0;

  /* ── Validate redirections before forking ────────────────────────── */
  if (!redirs_validate(start, NULL))
    return 0;

  /* ── Resolve executable ──────────────────────────────────────────── */
  char *resolved = resolve_command(argv[0]);
  if (resolved == NULL) {
    const char *display = (argv[0][0] == '%') ? argv[0] + 1 : argv[0];
    fprintf(stderr, "cshell: command not found (%s)\n", display);
    return 1;
  }

  /* ── Open redirections (plain fds, or feeder/tee helpers) ────────── */
  Redirs redirs;
  if (redirs_open(start, NULL, &redirs) != 0) {
    free(resolved);
    return 0;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    redirs_close(&redirs);
    redirs_wait(&redirs);
    free(resolved);
    return 0;
  }
  if (pid == 0) {
    setpgid(0, 0);              /* own group: ^C/^Z reach only this job */
    child_default_signals();
    redirs_install(&redirs);
    execve(resolved, argv, environ);
    perror("execve");
    _exit(1);
  }
  setpgid(pid, pid);            /* race-free: both sides set it */
  redirs_close(&redirs);        /* only the child holds them now */
  free(resolved);

  pid_t pids[1] = { pid };
  char  names[1][BG_NAME_MAX];
  strncpy(names[0], argv[0], BG_NAME_MAX - 1);
  names[0][BG_NAME_MAX - 1] = '\0';

  char cmdline[BG_CMD_MAX];
  token_group_to_string(start, cmdline, sizeof(cmdline));

  term_give(pid);
  /* A stopped job still holds its pipe ends, so its helpers cannot
     finish yet; check_bg_jobs reaps them later instead. */
  if (!fg_wait(pid, pids, names, 1, cmdline))
    redirs_wait(&redirs);
  return 0;
}

/* ================================================================== */
/* Pipeline support                                                    */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* extract_segment_argv: build argv from tokens [start, end).         */
/* Skips all operator tokens (< > >> |). Caller must free().           */
/* ------------------------------------------------------------------ */
static char **extract_segment_argv(Token *start, Token *end, int *out_argc) {
  int count = 0;
  int skip_next = 0;
  for (Token *t = start; t != end; t = t->next) {
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
  for (Token *t = start; t != end; t = t->next) {
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

/* ------------------------------------------------------------------ */
/* execute_pipeline: execute a chain of commands connected by pipes.   */
/* Tokens are scanned from start up to the first ; or & or end.        */
/* Always returns 0: spec C4 says a not-found stage "does not count as */
/* a failed command" for D1, so it must not stop a ; sequence.  Each   */
/* such stage already printed its own "command not found" error.       */
/* ------------------------------------------------------------------ */
int execute_pipeline(Token *start) {
  void (*old_handler)(int) = signal(SIGPIPE, SIG_IGN);

  /* ── 1. Count segments (commands) separated by | ──────────────────── */
  int n_cmds = 1;
  for (Token *t = start; t != NULL; t = t->next) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;
    if (t->type == TOKEN_OP_PIPE)
      n_cmds++;
  }

  /* ── 2. Build segment boundaries (start, end) for each command ───── */
  Token **seg_starts = malloc(n_cmds * sizeof(Token *));
  Token **seg_ends   = malloc(n_cmds * sizeof(Token *));
  if (seg_starts == NULL || seg_ends == NULL) {
    free(seg_starts);
    free(seg_ends);
    return 0;
  }

  int idx = 0;
  Token *t = start;
  seg_starts[0] = t;
  while (t != NULL) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;
    if (t->type == TOKEN_OP_PIPE) {
      seg_ends[idx] = t;
      idx++;
      seg_starts[idx] = t->next;
    }
    t = t->next;
  }
  seg_ends[idx] = t; /* end of last segment */

  /* ── 3. Validate redirections for each segment ────────────────────── */
  for (int i = 0; i < n_cmds; i++) {
    if (!redirs_validate(seg_starts[i], seg_ends[i])) {
      free(seg_starts);
      free(seg_ends);
      return 0;
    }
  }

  /* ── 4. Create pipes ─────────────────────────────────────────────── */
  int n_pipes = n_cmds - 1;
  int (*pipefds)[2] = NULL;
  if (n_pipes > 0) {
    pipefds = malloc(n_pipes * sizeof(int[2]));
    if (pipefds == NULL) {
      free(seg_starts);
      free(seg_ends);
      return 0;
    }
    for (int i = 0; i < n_pipes; i++) {
      if (pipe(pipefds[i]) < 0) {
        perror("pipe");
        for (int j = 0; j < i; j++) {
          close(pipefds[j][0]);
          close(pipefds[j][1]);
        }
        free(pipefds);
        free(seg_starts);
        free(seg_ends);
        return 0;
      }
    }
  }

  /* ── 5. Fork children ────────────────────────────────────────────── */
  pid_t *pids = malloc(n_cmds * sizeof(pid_t));
  char (*names)[BG_NAME_MAX] = malloc(n_cmds * BG_NAME_MAX);
  /* Per-stage redirections; zeroed so unused entries have no helpers. */
  Redirs *redirs = calloc(n_cmds, sizeof(Redirs));
  if (pids == NULL || names == NULL || redirs == NULL) {
    if (pipefds) {
      for (int i = 0; i < n_pipes; i++) {
        close(pipefds[i][0]);
        close(pipefds[i][1]);
      }
      free(pipefds);
    }
    free(pids);
    free(names);
    free(redirs);
    free(seg_starts);
    free(seg_ends);
    return 0;
  }

  /* All stages share one process group headed by the first, so the
     terminal can be handed to the pipeline as a unit. */
  pid_t pgid = 0;

  for (int i = 0; i < n_cmds; i++) {
    int argc = 0;
    names[i][0] = '\0';
    char **argv = extract_segment_argv(seg_starts[i], seg_ends[i], &argc);
    if (argv == NULL || argc == 0) {
      pids[i] = fork();
      if (pids[i] < 0) { perror("fork"); pids[i] = -1; free(argv); continue; }
      if (pids[i] == 0) { setpgid(0, pgid); _exit(0); }
      if (pgid == 0) pgid = pids[i];
      setpgid(pids[i], pgid);
      free(argv);
      continue;
    }
    strncpy(names[i], argv[0], BG_NAME_MAX - 1);
    names[i][BG_NAME_MAX - 1] = '\0';

    /* ── Check if this segment is a built-in command ─────────────── */
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

        pids[i] = fork();
        if (pids[i] < 0) { perror("fork"); pids[i] = -1; continue; }
        if (pids[i] == 0) { setpgid(0, pgid); _exit(127); }
        if (pgid == 0) pgid = pids[i];
        setpgid(pids[i], pgid);
        continue;
      }
    }

    /* Opened just before this stage's fork and closed right after, so
       no later stage inherits them. */
    if (redirs_open(seg_starts[i], seg_ends[i], &redirs[i]) != 0) {
      free(resolved);
      free(argv);
      pids[i] = -1;
      continue;
    }

    pids[i] = fork();
    if (pids[i] < 0) {
      perror("fork");
      redirs_close(&redirs[i]);
      free(resolved);
      free(argv);
      pids[i] = -1;
      continue;
    }

    if (pids[i] == 0) {
      /* ── Child ──────────────────────────────────────────────────── */
      setpgid(0, pgid);
      child_default_signals();
      if (i > 0)
        dup2(pipefds[i - 1][0], STDIN_FILENO);
      if (i < n_pipes)
        dup2(pipefds[i][1], STDOUT_FILENO);

      for (int j = 0; j < n_pipes; j++) {
        close(pipefds[j][0]);
        close(pipefds[j][1]);
      }

      /* File redirections override the pipe ends set up above. */
      redirs_install(&redirs[i]);

      if (is_builtin) {
        /* Run the built-in directly in this child process */
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
          HopEntry db[MAX_HOP_ENTRIES];
          int db_size = 0;
          load_hop_db(db, &db_size);
          hop(argc, argv, db, &db_size);
        }
        fflush(stdout);
        _exit(0);
      }

      execve(resolved, argv, environ);
      perror("execve");
      _exit(1);
    }

    if (pgid == 0) pgid = pids[i];
    setpgid(pids[i], pgid);
    redirs_close(&redirs[i]);

    free(resolved);
    free(argv);
  }

  /* ── 6. Parent: close all pipe fds, wait for all children ─────────── */
  if (pipefds) {
    for (int i = 0; i < n_pipes; i++) {
      close(pipefds[i][0]);
      close(pipefds[i][1]);
    }
  }

  /* Spec: give the pipeline the terminal before waiting on it, and take
     it back afterwards (fg_wait does the reclaim). */
  char cmdline[BG_CMD_MAX];
  token_group_to_string(start, cmdline, sizeof(cmdline));

  term_give(pgid);
  int stopped = fg_wait(pgid, pids, names, n_cmds, cmdline);

  /* Every stage has exited, so each tee has seen EOF and each feeder has
     finished or lost its reader.  A stopped job's helpers are still
     needed; check_bg_jobs reaps them later. */
  if (!stopped)
    for (int i = 0; i < n_cmds; i++)
      redirs_wait(&redirs[i]);

  /* ── 7. Cleanup ──────────────────────────────────────────────────── */
  signal(SIGPIPE, old_handler);
  free(pids);
  free(names);
  free(redirs);
  if (pipefds)
    free(pipefds);
  free(seg_starts);
  free(seg_ends);

  return 0;
}

/* ================================================================== */
/* Background pipeline support                                         */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* execute_pipeline_bg: like execute_pipeline, but the parent does not */
/* wait for children.  bg_child_setup keeps the stages off terminal    */
/* input (see bg.c).  Returns pid of the first command.                */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* record_bg_stage: remember one live pipeline stage for the job table. */
/* Copies the name, because argv[0] points into Token storage that      */
/* free_tokens() releases once the command finishes parsing.            */
/* ------------------------------------------------------------------ */
static void record_bg_stage(pid_t *out_pids, char (*out_names)[256],
                            int out_cap, int *rec, pid_t pid,
                            const char *name) {
  if (out_pids == NULL || out_names == NULL || *rec >= out_cap)
    return;
  out_pids[*rec] = pid;
  snprintf(out_names[*rec], 256, "%s", name);
  (*rec)++;
}

pid_t execute_pipeline_bg(Token *start, pid_t *out_pids,
                          char (*out_names)[256],
                          int out_cap, int *out_count) {
  int rec = 0;
  if (out_count != NULL)
    *out_count = 0;

  void (*old_handler)(int) = signal(SIGPIPE, SIG_IGN);

  /* ── 1. Count segments (commands) separated by | ─────────────────── */
  int n_cmds = 1;
  for (Token *t = start; t != NULL && t->type != TOKEN_OP_AMP; t = t->next) {
    if (t->type == TOKEN_OP_PIPE)
      n_cmds++;
  }

  /* ── 2. Build segment boundaries (start, end) for each command ───── */
  Token **seg_starts = malloc(n_cmds * sizeof(Token *));
  Token **seg_ends   = malloc(n_cmds * sizeof(Token *));
  if (seg_starts == NULL || seg_ends == NULL) {
    free(seg_starts);
    free(seg_ends);
    signal(SIGPIPE, old_handler);
    return 0;
  }

  int idx = 0;
  Token *t = start;
  seg_starts[0] = t;
  while (t != NULL && t->type != TOKEN_OP_AMP) {
    if (t->type == TOKEN_OP_PIPE) {
      seg_ends[idx] = t;
      idx++;
      seg_starts[idx] = t->next;
    }
    t = t->next;
  }
  seg_ends[idx] = t; /* end of last segment */

  /* ── 3. Validate redirections for each segment ───────────────────── */
  for (int i = 0; i < n_cmds; i++) {
    if (!redirs_validate(seg_starts[i], seg_ends[i])) {
      free(seg_starts);
      free(seg_ends);
      signal(SIGPIPE, old_handler);
      return 0;
    }
  }

  /* ── 4. Create pipes ─────────────────────────────────────────────── */
  int n_pipes = n_cmds - 1;
  int (*pipefds)[2] = NULL;
  if (n_pipes > 0) {
    pipefds = malloc(n_pipes * sizeof(int[2]));
    if (pipefds == NULL) {
      free(seg_starts);
      free(seg_ends);
      signal(SIGPIPE, old_handler);
      return 0;
    }
    for (int i = 0; i < n_pipes; i++) {
      if (pipe(pipefds[i]) < 0) {
        perror("pipe");
        for (int j = 0; j < i; j++) {
          close(pipefds[j][0]);
          close(pipefds[j][1]);
        }
        free(pipefds);
        free(seg_starts);
        free(seg_ends);
        signal(SIGPIPE, old_handler);
        return 0;
      }
    }
  }

  /* ── 5. Fork children ────────────────────────────────────────────── */
  pid_t *pids = malloc(n_cmds * sizeof(pid_t));
  if (pids == NULL) {
    if (pipefds) {
      for (int i = 0; i < n_pipes; i++) {
        close(pipefds[i][0]);
        close(pipefds[i][1]);
      }
      free(pipefds);
    }
    free(seg_starts);
    free(seg_ends);
    signal(SIGPIPE, old_handler);
    return 0;
  }

  /* Every stage of a background pipeline shares one process group,
     headed by the first command, so terminal signals skip all of them. */
  pid_t pgid = 0;

  for (int i = 0; i < n_cmds; i++) {
    int argc = 0;
    char **argv = extract_segment_argv(seg_starts[i], seg_ends[i], &argc);
    if (argv == NULL || argc == 0) {
      pids[i] = fork();
      if (pids[i] < 0) { perror("fork"); pids[i] = -1; free(argv); continue; }
      if (pids[i] == 0) { setpgid(0, pgid); _exit(0); }
      if (pgid == 0) pgid = pids[i];
      setpgid(pids[i], pgid);
      record_bg_stage(out_pids, out_names, out_cap, &rec, pids[i], "?");
      free(argv);
      continue;
    }

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

        /* Copy the name BEFORE freeing argv: the recording below would
           otherwise read freed memory. */
        char nf_name[256];
        strncpy(nf_name, argv[0], sizeof(nf_name) - 1);
        nf_name[sizeof(nf_name) - 1] = '\0';
        free(argv);

        pids[i] = fork();
        if (pids[i] < 0) { perror("fork"); pids[i] = -1; continue; }
        if (pids[i] == 0) { setpgid(0, pgid); _exit(127); }
        if (pgid == 0) pgid = pids[i];
        setpgid(pids[i], pgid);
        record_bg_stage(out_pids, out_names, out_cap, &rec, pids[i], nf_name);
        continue;
      }
    }

    /* Nobody waits on a background job, so its feeder/tee helpers are
       reaped by check_bg_jobs, which ignores pids it does not track. */
    Redirs redirs;
    if (redirs_open(seg_starts[i], seg_ends[i], &redirs) != 0) {
      free(resolved);
      free(argv);
      pids[i] = -1;
      continue;
    }

    pids[i] = fork();
    if (pids[i] < 0) {
      perror("fork");
      redirs_close(&redirs);
      free(resolved);
      free(argv);
      pids[i] = -1;
      continue;
    }

    if (pids[i] == 0) {
      /* ── Child ──────────────────────────────────────────────────── */
      setpgid(0, pgid);
      /* Undo the SIGPIPE ignore above, which would survive execve. */
      signal(SIGPIPE, SIG_DFL);
      bg_child_setup();

      if (i > 0)
        dup2(pipefds[i - 1][0], STDIN_FILENO);
      if (i < n_pipes)
        dup2(pipefds[i][1], STDOUT_FILENO);

      for (int j = 0; j < n_pipes; j++) {
        close(pipefds[j][0]);
        close(pipefds[j][1]);
      }

      redirs_install(&redirs);

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
          HopEntry db[MAX_HOP_ENTRIES];
          int db_size = 0;
          load_hop_db(db, &db_size);
          hop(argc, argv, db, &db_size);
        }
        fflush(stdout);
        _exit(0);
      }

      execve(resolved, argv, environ);
      perror("execve");
      _exit(1);
    }

    if (pgid == 0) pgid = pids[i];
    setpgid(pids[i], pgid);
    redirs_close(&redirs);
    record_bg_stage(out_pids, out_names, out_cap, &rec, pids[i], argv[0]);

    free(resolved);
    free(argv);
  }

  /* ── 6. Parent: close all pipe fds, do NOT wait ─────────────────── */
  if (pipefds) {
    for (int i = 0; i < n_pipes; i++) {
      close(pipefds[i][0]);
      close(pipefds[i][1]);
    }
  }

  signal(SIGPIPE, old_handler);

  /* out_pids[0] is the first stage that actually forked, and pgid was set
     from that same pid, so the two always agree. */
  if (out_count != NULL)
    *out_count = rec;
  pid_t result_pgid = (rec > 0) ? pgid : 0;

  /* ── 7. Cleanup ──────────────────────────────────────────────────── */
  free(pids);
  if (pipefds)
    free(pipefds);
  free(seg_starts);
  free(seg_ends);

  return result_pgid;
}
