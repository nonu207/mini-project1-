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

  /* The job owns the terminal now, so ^C goes to it and never to the
     shell -- the shell must therefore end the "^C" line itself, or the
     next line of output runs on from it. */
  if (interrupted) {
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
      snprintf(fullpath, sizeof(fullpath), "%s/%s", cwd, lookup);
      if (check_executable(fullpath))
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

/* ------------------------------------------------------------------ */
/* Helper: check if token list (from start up to ; or &) has an op.   */
/* ------------------------------------------------------------------ */
static int has_operator(Token *start, TokenType op) {
  for (Token *t = start; t != NULL; t = t->next) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;
    if (t->type == op)
      return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* validate_input_redirections: check that all < target files exist.  */
/* Returns 1 if all OK, 0 if any file is missing.                     */
/* ------------------------------------------------------------------ */
static int validate_input_redirections(Token *start) {
  Token *t = start;
  while (t != NULL) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;
    if (t->type == TOKEN_OP_LT) {
      t = t->next;
      if (t == NULL || t->type != TOKEN_WORD)
        return 0;
      int fd = open(t->value, O_RDONLY);
      if (fd < 0) {
        fprintf(stderr, "cshell: no such file or directory\n");
        return 0;
      }
      close(fd);
    }
    t = t->next;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* validate_output_redirections: check all > / >> files are writable. */
/* Returns 1 if all OK, 0 if any file cannot be opened.               */
/* ------------------------------------------------------------------ */
static int validate_output_redirections(Token *start) {
  Token *t = start;
  while (t != NULL) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;
    if (t->type == TOKEN_OP_GT || t->type == TOKEN_OP_GTGT) {
      Token *op = t;
      t = t->next;
      if (t == NULL || t->type != TOKEN_WORD)
        return 0;
      int flags = O_WRONLY | O_CREAT;
      if (op->type == TOKEN_OP_GTGT)
        flags |= O_APPEND;
      else
        flags |= O_TRUNC;
      int fd = open(t->value, flags, 0644);
      if (fd < 0) {
        fprintf(stderr, "cshell: unable to create file for writing\n");
        return 0;
      }
      close(fd);
    }
    t = t->next;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* feed_input_files: write all < file contents into pipe write-end.   */
/* ------------------------------------------------------------------ */
static void feed_input_files(Token *start, int pipe_wr) {
  Token *t = start;
  while (t != NULL) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;
    if (t->type == TOKEN_OP_LT) {
      t = t->next;
      if (t == NULL || t->type != TOKEN_WORD)
        break;
      int fd = open(t->value, O_RDONLY);
      if (fd < 0)
        break;
      char buf[4096];
      ssize_t n;
      while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(pipe_wr, buf, (size_t)n);
      close(fd);
    }
    t = t->next;
  }
}

/* ------------------------------------------------------------------ */
/* write_output_files: write captured output to all > / >> targets.   */
/* ------------------------------------------------------------------ */
static void write_output_files(Token *start,
                               const char *data, size_t len) {
  Token *t = start;
  while (t != NULL) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;
    if (t->type == TOKEN_OP_GT || t->type == TOKEN_OP_GTGT) {
      Token *op = t;
      t = t->next;
      if (t == NULL || t->type != TOKEN_WORD)
        break;
      int flags = O_WRONLY | O_CREAT;
      if (op->type == TOKEN_OP_GTGT)
        flags |= O_APPEND;
      else
        flags |= O_TRUNC;
      int fd = open(t->value, flags, 0644);
      if (fd >= 0) {
        if (len > 0)
          write(fd, data, len);
        close(fd);
      }
    }
    t = t->next;
  }
}

/* ------------------------------------------------------------------ */
/* execute_command: fork + execve an external command.                 */
/* Returns 0 on success, 1 if the command was not found.               */
/* ------------------------------------------------------------------ */
int execute_command(int argc, char **argv, Token *start) {
  if (argc < 1 || argv[0] == NULL)
    return 0;

  int has_input  = has_operator(start, TOKEN_OP_LT);
  int has_output = has_operator(start, TOKEN_OP_GT) ||
                   has_operator(start, TOKEN_OP_GTGT);

  /* ── Validate redirections before forking ────────────────────────── */
  if (has_input && !validate_input_redirections(start))
    return 0;
  if (has_output && !validate_output_redirections(start))
    return 0;

  /* ── Resolve executable ──────────────────────────────────────────── */
  char *resolved = resolve_command(argv[0]);
  if (resolved == NULL) {
    const char *display = (argv[0][0] == '%') ? argv[0] + 1 : argv[0];
    fprintf(stderr, "cshell: command not found (%s)\n", display);
    return 1;
  }

  /* ── No redirections: simple fork + exec ─────────────────────────── */
  if (!has_input && !has_output) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      free(resolved);
      return 0;
    }
    if (pid == 0) {
      setpgid(0, 0);              /* own group: ^C/^Z reach only this job */
      child_default_signals();
      execve(resolved, argv, environ);
      perror("execve");
      _exit(1);
    }
    setpgid(pid, pid);            /* race-free: both sides set it */
    free(resolved);

    pid_t pids[1] = { pid };
    char  names[1][BG_NAME_MAX];
    strncpy(names[0], argv[0], BG_NAME_MAX - 1);
    names[0][BG_NAME_MAX - 1] = '\0';

    char cmdline[BG_CMD_MAX];
    token_group_to_string(start, cmdline, sizeof(cmdline));

    term_give(pid);
    fg_wait(pid, pids, names, 1, cmdline);
    return 0;
  }

  /* ── Set up pipes ────────────────────────────────────────────────── */
  int in_pipe[2]  = {-1, -1};
  int out_pipe[2] = {-1, -1};

  if (has_input && pipe(in_pipe) < 0) {
    perror("pipe");
    free(resolved);
    return 0;
  }
  if (has_output && pipe(out_pipe) < 0) {
    perror("pipe");
    if (has_input) { close(in_pipe[0]); close(in_pipe[1]); }
    free(resolved);
    return 0;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    if (has_input)  { close(in_pipe[0]);  close(in_pipe[1]);  }
    if (has_output) { close(out_pipe[0]); close(out_pipe[1]); }
    free(resolved);
    return 0;
  }

  if (pid == 0) {
    /* ── Child ──────────────────────────────────────────────────────── */
    setpgid(0, 0);
    child_default_signals();
    if (has_input) {
      close(in_pipe[1]);
      dup2(in_pipe[0], STDIN_FILENO);
      close(in_pipe[0]);
    }
    if (has_output) {
      close(out_pipe[0]);
      dup2(out_pipe[1], STDOUT_FILENO);
      close(out_pipe[1]);
    }
    execve(resolved, argv, environ);
    perror("execve");
    _exit(1);
  }

  /* ── Parent ──────────────────────────────────────────────────────── */
  setpgid(pid, pid);
  term_give(pid);
  free(resolved);

  /* Feed input files to child's stdin */
  if (has_input) {
    close(in_pipe[0]);
    feed_input_files(start, in_pipe[1]);
    close(in_pipe[1]);
  }

  /* Capture child's stdout and write to all output files */
  if (has_output) {
    close(out_pipe[1]);
    size_t cap = 0;
    size_t len = 0;
    char *buf = NULL;
    char chunk[4096];
    ssize_t n;
    while ((n = read(out_pipe[0], chunk, sizeof(chunk))) > 0) {
      if (len + (size_t)n > cap) {
        size_t new_cap = (cap == 0) ? 4096 : cap * 2;
        while (new_cap < len + (size_t)n)
          new_cap *= 2;
        char *tmp = realloc(buf, new_cap);
        if (tmp == NULL) { free(buf); close(out_pipe[0]); return 0; }
        buf = tmp;
        cap = new_cap;
      }
      memcpy(buf + len, chunk, (size_t)n);
      len += (size_t)n;
    }
    close(out_pipe[0]);
    write_output_files(start, buf, len);
    free(buf);
  }

  pid_t fg_pids[1] = { pid };
  char  fg_names[1][BG_NAME_MAX];
  strncpy(fg_names[0], argv[0], BG_NAME_MAX - 1);
  fg_names[0][BG_NAME_MAX - 1] = '\0';
  char fg_cmdline[BG_CMD_MAX];
  token_group_to_string(start, fg_cmdline, sizeof(fg_cmdline));
  fg_wait(pid, fg_pids, fg_names, 1, fg_cmdline);
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
/* validate_segment_redirs: validate < and > files for one segment.   */
/* Returns 1 if OK, 0 if any file fails.                              */
/* ------------------------------------------------------------------ */
static int validate_segment_redirs(Token *start, Token *end) {
  for (Token *t = start; t != end; t = t->next) {
    if (t->type == TOKEN_OP_LT) {
      t = t->next;
      if (t == end || t->type != TOKEN_WORD)
        return 0;
      int fd = open(t->value, O_RDONLY);
      if (fd < 0) {
        fprintf(stderr, "cshell: no such file or directory\n");
        return 0;
      }
      close(fd);
    } else if (t->type == TOKEN_OP_GT || t->type == TOKEN_OP_GTGT) {
      Token *op = t;
      t = t->next;
      if (t == end || t->type != TOKEN_WORD)
        return 0;
      int flags = O_WRONLY | O_CREAT;
      if (op->type == TOKEN_OP_GTGT)
        flags |= O_APPEND;
      else
        flags |= O_TRUNC;
      int fd = open(t->value, flags, 0644);
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
/* apply_segment_redirs: in child process, set up < and > for one     */
/* segment. Returns 0 on success, -1 on error (child should exit).    */
/* ------------------------------------------------------------------ */
static int apply_segment_redirs(Token *start, Token *end) {
  for (Token *t = start; t != end; t = t->next) {
    if (t->type == TOKEN_OP_LT) {
      t = t->next;
      if (t == end || t->type != TOKEN_WORD)
        return -1;
      int fd = open(t->value, O_RDONLY);
      if (fd < 0)
        return -1;
      dup2(fd, STDIN_FILENO);
      close(fd);
    } else if (t->type == TOKEN_OP_GT || t->type == TOKEN_OP_GTGT) {
      Token *op = t;
      t = t->next;
      if (t == end || t->type != TOKEN_WORD)
        return -1;
      int flags = O_WRONLY | O_CREAT;
      if (op->type == TOKEN_OP_GTGT)
        flags |= O_APPEND;
      else
        flags |= O_TRUNC;
      int fd = open(t->value, flags, 0644);
      if (fd < 0)
        return -1;
      dup2(fd, STDOUT_FILENO);
      close(fd);
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* execute_pipeline: execute a chain of commands connected by pipes.   */
/* Tokens are scanned from start up to the first ; or & or end.        */
/* Returns 0 if every stage was found, 1 if any stage was not found.   */
/* ------------------------------------------------------------------ */
int execute_pipeline(Token *start) {
  void (*old_handler)(int) = signal(SIGPIPE, SIG_IGN);
  int all_found = 1;

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
    if (!validate_segment_redirs(seg_starts[i], seg_ends[i])) {
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
  if (pids == NULL || names == NULL) {
    if (pipefds) {
      for (int i = 0; i < n_pipes; i++) {
        close(pipefds[i][0]);
        close(pipefds[i][1]);
      }
      free(pipefds);
    }
    free(pids);
    free(names);
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
                    strcmp(argv[0], "resume") == 0);

    char *resolved = NULL;
    if (!is_builtin) {
      resolved = resolve_command(argv[0]);
      if (resolved == NULL) {
        const char *display = (argv[0][0] == '%') ? argv[0] + 1 : argv[0];
        fprintf(stderr, "cshell: command not found (%s)\n", display);
        all_found = 0;
        free(argv);

        pids[i] = fork();
        if (pids[i] < 0) { perror("fork"); pids[i] = -1; continue; }
        if (pids[i] == 0) { setpgid(0, pgid); _exit(127); }
        if (pgid == 0) pgid = pids[i];
        setpgid(pids[i], pgid);
        continue;
      }
    }

    pids[i] = fork();
    if (pids[i] < 0) {
      perror("fork");
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

      if (apply_segment_redirs(seg_starts[i], seg_ends[i]) < 0)
        _exit(1);

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
  fg_wait(pgid, pids, names, n_cmds, cmdline);

  /* ── 7. Cleanup ──────────────────────────────────────────────────── */
  signal(SIGPIPE, old_handler);
  free(pids);
  free(names);
  if (pipefds)
    free(pipefds);
  free(seg_starts);
  free(seg_ends);

  return all_found ? 0 : 1;
}

/* ================================================================== */
/* Background pipeline support                                         */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* execute_pipeline_bg: like execute_pipeline, but the parent does not */
/* wait for children.  stdin of every child is /dev/null so the        */
/* pipeline has no terminal access.  Returns pid of the first command. */
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
  strncpy(out_names[*rec], name, 255);
  out_names[*rec][255] = '\0';
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
    if (!validate_segment_redirs(seg_starts[i], seg_ends[i])) {
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

    pids[i] = fork();
    if (pids[i] < 0) {
      perror("fork");
      free(resolved);
      free(argv);
      pids[i] = -1;
      continue;
    }

    if (pids[i] == 0) {
      /* ── Child ──────────────────────────────────────────────────── */
      setpgid(0, pgid);

      int devnull = open("/dev/null", O_RDONLY);
      if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        close(devnull);
      }

      if (i > 0)
        dup2(pipefds[i - 1][0], STDIN_FILENO);
      if (i < n_pipes)
        dup2(pipefds[i][1], STDOUT_FILENO);

      for (int j = 0; j < n_pipes; j++) {
        close(pipefds[j][0]);
        close(pipefds[j][1]);
      }

      if (apply_segment_redirs(seg_starts[i], seg_ends[i]) < 0)
        _exit(1);

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
