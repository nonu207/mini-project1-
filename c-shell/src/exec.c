#include "shell.h"

extern char **environ;

/* Restores default signal handling in a forked job, since it must
 * react to the keyboard normally even though the shell itself ignores
 * or specially handles these signals, and an ignored or handled signal
 * would otherwise survive execve into the job. SIGPIPE is included
 * because the shell ignores it while wiring up a pipeline; without
 * restoring it here, a stage whose reader has already gone, such as
 * "yes | head -1", would print a broken pipe error instead of simply
 * exiting quietly. */
static void child_default_signals(void) {
  signal(SIGINT,  SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGTTOU, SIG_DFL);
  signal(SIGTTIN, SIG_DFL);
  signal(SIGPIPE, SIG_DFL);
}

/* Waits for an entire foreground job to finish or stop, then reclaims
 * the terminal. WUNTRACED is what makes Ctrl-Z observable here, since
 * without it a stopped child would never cause waitpid to return and
 * the shell would simply hang.
 *
 * If the group stopped, it is handed to the job table so it gets a job
 * number, shows up in activities, and blocks Ctrl-D. Only the
 * processes that actually stopped are recorded, since a stage that had
 * already exited must not be resurrected as a stopped process.
 *
 * Returns 1 if the job stopped, 0 if every process in it finished. */
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
      /* Survivors are compacted to the front, keeping pipeline order. */
      pids[sn] = pids[i];
      if (sn != i)
        memcpy(names[sn], names[i], BG_NAME_MAX);
      sn++;
    } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) {
      interrupted = 1;
    }
  }

  term_take();

  /* The job owned the terminal while it ran, so a Ctrl-C or Ctrl-Z
   * keystroke went to it and never reached the shell directly. The
   * shell must therefore end the echoed "^C" or "^Z" line itself here,
   * or the next line of output would run on from it, producing
   * something like "^Z[1] + Stopped ...". A stop is only echoed this
   * way on a real terminal. */
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

/* Reports whether a path is an executable regular file. */
static int check_executable(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)    return 0;
  if (S_ISDIR(st.st_mode))     return 0;
  if (access(path, X_OK) != 0) return 0;
  return 1;
}

/* Resolves a command name to an executable path, following the rules
 * of C1: a name containing a slash is treated as a literal path; a
 * leading percent sign skips the current-directory lookup and searches
 * PATH directly; otherwise the current directory is checked first,
 * then PATH, in order. Returns a heap-allocated string that the caller
 * must free, or NULL if nothing executable was found. */
char *resolve_command(const char *name) {
  if (name == NULL || name[0] == '\0')
    return NULL;

  char fullpath[PATH_MAX];
  int name_has_slash = (strchr(name, '/') != NULL);
  int percent_skip = (name[0] == '%');
  const char *lookup = percent_skip ? name + 1 : name;

  if (name_has_slash) {
    if (check_executable(name))
      return strdup(name);
    return NULL;
  }

  if (!percent_skip) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      /* A path that would be truncated to fit PATH_MAX is skipped
       * rather than probed with a name it does not actually have. */
      int len = snprintf(fullpath, sizeof(fullpath), "%s/%s", cwd, lookup);
      if (len >= 0 && (size_t)len < sizeof(fullpath) &&
          check_executable(fullpath))
        return strdup(fullpath);
    }
  }

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

/* Redirection support.
 *
 * A single input or output redirection is just a file opened in the
 * shell and dup2'd onto the command's stdin or stdout. Several input
 * files are joined into one stream by a small feeder process that
 * writes them, in order, into a pipe, and several output files are
 * filled by a small tee process that copies a pipe into each of them.
 * These helper processes run alongside the command itself, so the
 * shell never has to pump the data through by hand and cannot
 * deadlock, and output reaches the files as it is produced rather than
 * only once the command finishes. */

/* Reports whether a token ends the scan of a command's tokens between
 * start and end. */
static int is_redir_end(Token *t, Token *end) {
  return t == NULL || t == end ||
         t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP;
}

/* Returns the open() flags for a redirection target: truncate for a
 * single greater-than, append for a double greater-than. */
static int output_flags(TokenType op) {
  return O_WRONLY | O_CREAT | (op == TOKEN_OP_GTGT ? O_APPEND : O_TRUNC);
}

/* Closes every file descriptor at or above a given number. A helper
 * process must hold no pipe end besides its own, or a reader elsewhere
 * would never see end of file. */
static void close_fds_from(int low) {
  long max = sysconf(_SC_OPEN_MAX);
  if (max < 0 || max > 4096)
    max = 4096;
  for (int fd = low; fd < max; fd++)
    close(fd);
}

/* Writes an entire buffer to a file descriptor, retrying on a short
 * write or an interrupted call. */
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

/* Checks every redirection target between start and end, printing the
 * appropriate error and returning 0 on the first one that cannot be
 * opened. A greater-than target is actually created or truncated here
 * as part of the check. Returns 1 if every target is usable. */
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

/* Forks a feeder process that writes every input file between start
 * and end, in order, into a pipe. Returns the pipe's read end, meant
 * to become the command's stdin, and sets pid_out to the feeder's pid,
 * or returns -1 on failure. */
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
    /* If the command stops reading, this process should simply stop
     * too rather than print an error. */
    signal(SIGPIPE, SIG_DFL);
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

/* Forks a tee process that copies a pipe into every output file
 * between start and end, each opened with its own truncate or append
 * mode. Returns the pipe's write end, meant to become the command's
 * stdout, and sets pid_out to the tee's pid, or returns -1 on
 * failure. */
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
      /* A file that fails a write is dropped from the list; the rest
       * keep receiving data. */
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

/* Opens the redirections of a command between start and end, filling
 * in r. A single input or output file becomes a plain file descriptor;
 * more than one of either kind is handled by forking a feeder or a tee
 * helper. Must be called after redirs_validate. Returns 0 on success,
 * or -1 on failure, in which case nothing is left open. */
int redirs_open(Token *start, Token *end, Redirs *r) {
  r->in_fd = -1;
  r->out_fd = -1;
  r->helper[0] = 0;
  r->helper[1] = 0;

  /* The targets are counted first, remembering the last one of each
   * kind for the single-file case. */
  int n_in = 0, n_out = 0;
  Token *last_in = NULL;
  Token *last_out = NULL;
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
    /* Closing the input side here lets a feeder see its reader is gone
     * and exit on its own. */
    redirs_close(r);
    redirs_wait(r);
    r->helper[0] = 0;
    r->helper[1] = 0;
    return -1;
  }
  return 0;
}

/* Installs an opened redirection set onto stdin and stdout, closing
 * the originals afterward. */
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

/* Closes an opened redirection set's file descriptors without
 * installing them, for example in a parent process after forking. */
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

/* Waits for a redirection set's feeder and tee helper processes to
 * finish. Must only be called once every holder of the redirection
 * file descriptors has closed them, or a tee process would never see
 * end of file and this would hang. */
void redirs_wait(const Redirs *r) {
  for (int i = 0; i < 2; i++) {
    if (r->helper[i] <= 0)
      continue;
    while (waitpid(r->helper[i], NULL, 0) < 0 && errno == EINTR)
      ;
  }
}

/* Runs a single external command with fork and execve, applying its
 * redirections. Returns 0 on success, or 1 if the command could not be
 * found. */
int execute_command(int argc, char **argv, Token *start) {
  if (argc < 1 || argv[0] == NULL)
    return 0;

  if (!redirs_validate(start, NULL))
    return 0;

  char *resolved = resolve_command(argv[0]);
  if (resolved == NULL) {
    const char *display = (argv[0][0] == '%') ? argv[0] + 1 : argv[0];
    fprintf(stderr, "cshell: command not found (%s)\n", display);
    return 1;
  }

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
    /* This job gets its own process group, so Ctrl-C and Ctrl-Z reach
     * only it. */
    setpgid(0, 0);
    child_default_signals();
    redirs_install(&redirs);
    execve(resolved, argv, environ);
    perror("execve");
    _exit(1);
  }
  /* Both the parent and the child set the group, so this is race-free
   * regardless of which side runs first. */
  setpgid(pid, pid);
  /* Only the child holds these descriptors now. */
  redirs_close(&redirs);
  free(resolved);

  pid_t pids[1] = { pid };
  char  names[1][BG_NAME_MAX];
  strncpy(names[0], argv[0], BG_NAME_MAX - 1);
  names[0][BG_NAME_MAX - 1] = '\0';

  char cmdline[BG_CMD_MAX];
  token_group_to_string(start, cmdline, sizeof(cmdline));

  term_give(pid);
  /* A stopped job still holds its own pipe ends, so its helpers cannot
   * finish yet; check_bg_jobs reaps them later instead. */
  if (!fg_wait(pid, pids, names, 1, cmdline))
    redirs_wait(&redirs);
  return 0;
}

/* Builds a NULL-terminated argv array from the tokens of one pipeline
 * segment between start and end, skipping all operator tokens. The
 * caller must free the result. */
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

/* Runs a chain of commands connected by pipes, scanning tokens from
 * start up to the first semicolon, ampersand, or the end of input.
 *
 * Always returns 0. Per the spec's C4 requirement, a stage whose
 * command is not found does not count as a failed command for the D1
 * sequencing rule, so it must never stop a semicolon-separated
 * sequence; that stage has already printed its own command-not-found
 * error before this function returns. */
int execute_pipeline(Token *start) {
  void (*old_handler)(int) = signal(SIGPIPE, SIG_IGN);

  int n_cmds = 1;
  for (Token *t = start; t != NULL; t = t->next) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;
    if (t->type == TOKEN_OP_PIPE)
      n_cmds++;
  }

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
  seg_ends[idx] = t;

  for (int i = 0; i < n_cmds; i++) {
    if (!redirs_validate(seg_starts[i], seg_ends[i])) {
      free(seg_starts);
      free(seg_ends);
      return 0;
    }
  }

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

  pid_t *pids = malloc(n_cmds * sizeof(pid_t));
  char (*names)[BG_NAME_MAX] = malloc(n_cmds * BG_NAME_MAX);
  /* Zeroed so any stage not given its own redirections has no
   * helpers. */
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
   * terminal can be handed to the whole pipeline as a unit. */
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

    /* This stage's redirections are opened just before its fork and
     * closed right after, so no later stage inherits them. */
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

      /* Any file redirections this stage has override the pipe ends
       * set up above. */
      redirs_install(&redirs[i]);

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
    redirs_close(&redirs[i]);

    free(resolved);
    free(argv);
  }

  if (pipefds) {
    for (int i = 0; i < n_pipes; i++) {
      close(pipefds[i][0]);
      close(pipefds[i][1]);
    }
  }

  /* The pipeline is given the terminal before it is waited on, and
   * fg_wait reclaims it afterward. */
  char cmdline[BG_CMD_MAX];
  token_group_to_string(start, cmdline, sizeof(cmdline));

  term_give(pgid);
  int stopped = fg_wait(pgid, pids, names, n_cmds, cmdline);

  /* Every stage has now exited, so each tee has seen end of file and
   * each feeder has either finished or lost its reader. A stopped
   * job's helpers are still needed and are reaped later by
   * check_bg_jobs instead. */
  if (!stopped)
    for (int i = 0; i < n_cmds; i++)
      redirs_wait(&redirs[i]);

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

/* Records one live pipeline stage into the caller's output arrays for
 * the job table. The name is copied because argv[0] points into token
 * storage that free_tokens releases once the command line has finished
 * parsing. */
static void record_bg_stage(pid_t *out_pids, char (*out_names)[256],
                            int out_cap, int *rec, pid_t pid,
                            const char *name) {
  if (out_pids == NULL || out_names == NULL || *rec >= out_cap)
    return;
  out_pids[*rec] = pid;
  snprintf(out_names[*rec], 256, "%s", name);
  (*rec)++;
}

/* Runs a chain of commands connected by pipes in the background, the
 * same way execute_pipeline does, except that the parent never waits
 * for any of the children. bg_child_setup keeps each stage off
 * terminal input, as described in bg.c. Returns the pid of the first
 * command, which also serves as the pipeline's process group id, or 0
 * on error. out_pids and out_names are filled with the pid and name of
 * each stage that actually launched, in order, and out_count is set to
 * how many that was. */
pid_t execute_pipeline_bg(Token *start, pid_t *out_pids,
                          char (*out_names)[256],
                          int out_cap, int *out_count) {
  int rec = 0;
  if (out_count != NULL)
    *out_count = 0;

  void (*old_handler)(int) = signal(SIGPIPE, SIG_IGN);

  int n_cmds = 1;
  for (Token *t = start; t != NULL && t->type != TOKEN_OP_AMP; t = t->next) {
    if (t->type == TOKEN_OP_PIPE)
      n_cmds++;
  }

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
  seg_ends[idx] = t;

  for (int i = 0; i < n_cmds; i++) {
    if (!redirs_validate(seg_starts[i], seg_ends[i])) {
      free(seg_starts);
      free(seg_ends);
      signal(SIGPIPE, old_handler);
      return 0;
    }
  }

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
   * headed by the first command, so terminal signals skip all of
   * them. */
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

        /* The name is copied before argv is freed, since recording it
         * afterward would read already-freed memory. */
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

    /* Nobody waits on a background job directly, so its feeder and
     * tee helpers are reaped later by check_bg_jobs, which simply
     * ignores any pid it does not track. */
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
      setpgid(0, pgid);
      /* Undoes the SIGPIPE ignore set above, which would otherwise
       * survive execve. */
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

  if (pipefds) {
    for (int i = 0; i < n_pipes; i++) {
      close(pipefds[i][0]);
      close(pipefds[i][1]);
    }
  }

  signal(SIGPIPE, old_handler);

  /* out_pids[0] is the first stage that actually forked, and pgid was
   * set from that very pid, so the two always agree. */
  if (out_count != NULL)
    *out_count = rec;
  pid_t result_pgid = (rec > 0) ? pgid : 0;

  free(pids);
  if (pipefds)
    free(pipefds);
  free(seg_starts);
  free(seg_ends);

  return result_pgid;
}
