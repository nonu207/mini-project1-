#include "shell.h"

extern char **environ;

/* ── Background job table ──────────────────────────────────────────── */
#define MAX_BG_JOBS 256

static BgJob bg_jobs[MAX_BG_JOBS];
static int   bg_job_count = 0;

/* ── SIGCHLD handler ───────────────────────────────────────────────── */
/* A no-op handler is required so SIGCHLD interrupts blocking system    */
/* calls (fgets in main, waitpid in exec) with EINTR instead of being   */
/* silently ignored.  No SA_RESTART: this is what wakes the shell while */
/* waiting for user input so it can report background completions.      */
/* All reaping + reporting happens in check_bg_jobs (main context).     */
/* ------------------------------------------------------------------- */
static void sigchld_handler(int sig) {
  (void)sig;
}

/* ── Public API ────────────────────────────────────────────────────── */

void init_bg(void) {
  for (int i = 0; i < MAX_BG_JOBS; i++) {
    bg_jobs[i].pid = 0;
    bg_jobs[i].job_number = 0;
    bg_jobs[i].command_name[0] = '\0';
  }

  struct sigaction sa;
  sa.sa_handler = sigchld_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);
}

void register_bg_job(pid_t pid, const char *name) {
  for (int i = 0; i < MAX_BG_JOBS; i++) {
    if (bg_jobs[i].pid == 0) {
      bg_jobs[i].pid = pid;
      bg_jobs[i].job_number = ++bg_job_count;
      if (name != NULL) {
        strncpy(bg_jobs[i].command_name, name,
                sizeof(bg_jobs[i].command_name) - 1);
        bg_jobs[i].command_name[sizeof(bg_jobs[i].command_name) - 1] = '\0';
      }
      /* Spec: print "[job_number] process_id" to stdout, before any
         output the command produces. */
      printf("[%d] %d\n", bg_jobs[i].job_number, (int)pid);
      fflush(stdout);
      return;
    }
  }
  fprintf(stderr, "cshell: too many background jobs\n");
}

void check_bg_jobs(void) {
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    for (int i = 0; i < MAX_BG_JOBS; i++) {
      if (bg_jobs[i].pid == pid) {
        /* Spec: print to stdout (same stream as the prompt and [N] pid).
           Format: "<name> with pid <pid> exited normally"   (WIFEXITED)
                   "<name> with pid <pid> exited abnormally" (WIFSIGNALED)
           Note: NO trailing period — the spec examples have none.     */
        if (WIFEXITED(status)) {
          printf("%s with pid %d exited normally\n",
                 bg_jobs[i].command_name, (int)pid);
        } else if (WIFSIGNALED(status)) {
          printf("%s with pid %d exited abnormally\n",
                 bg_jobs[i].command_name, (int)pid);
        }
        fflush(stdout);
        bg_jobs[i].pid = 0;
        break;
      }
    }
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
    pid_t pid = execute_pipeline_bg(start);
    if (pid > 0) {
      const char *name = start->value;
      register_bg_job(pid, name);
    }
    return 0;
  }

  int argc = 0;
  char **argv = extract_bg_argv(start, &argc);
  if (argv == NULL || argc == 0) {
    free(argv);
    return 0;
  }

  char *resolved = resolve_command(argv[0]);
  if (resolved == NULL) {
    const char *display = (argv[0][0] == '%') ? argv[0] + 1 : argv[0];
    fprintf(stderr, "cshell: command not found (%s)\n", display);
    free(argv);
    return 0;
  }

  int is_builtin = (strcmp(argv[0], "peek") == 0 ||
                    strcmp(argv[0], "reveal") == 0 ||
                    strcmp(argv[0], "locate") == 0 ||
                    strcmp(argv[0], "hop") == 0);

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    free(resolved);
    free(argv);
    return 0;
  }

  if (pid == 0) {
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      close(devnull);
    }

    SavedFds saved;
    if (apply_redirections(start, &saved) < 0)
      _exit(1);

    if (is_builtin) {
      if (strcmp(argv[0], "peek") == 0) {
        peek(argc, argv);
      } else if (strcmp(argv[0], "reveal") == 0) {
        reveal(argc, argv);
      } else if (strcmp(argv[0], "locate") == 0) {
        locate(argc, argv);
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

  register_bg_job(pid, argv[0]);
  free(resolved);
  free(argv);
  return 0;
}