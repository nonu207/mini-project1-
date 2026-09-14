#include "shell.h"

/* ------------------------------------------------------------------ */
/* Build a NULL-terminated argv[] from ONE command group only.         */
/* Stops at the first operator ( ; & < > >> | ) or end of list.        */
/* Caller must free() the returned pointer.                            */
/* ------------------------------------------------------------------ */
static char **extract_group(Token *start, int *out_argc) {
  /* Count words in this command group (before any operator) */
  int word_count = 0;
  Token *t = start;
  while (t != NULL) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP ||
        t->type == TOKEN_OP_LT  || t->type == TOKEN_OP_GT  ||
        t->type == TOKEN_OP_GTGT || t->type == TOKEN_OP_PIPE)
      break;
    if (t->type == TOKEN_WORD)
      word_count++;
    t = t->next;
  }

  char **argv = malloc((word_count + 1) * sizeof(char *));
  if (argv == NULL)
    return NULL;

  int i = 0;
  t = start;
  while (t != NULL) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP ||
        t->type == TOKEN_OP_LT  || t->type == TOKEN_OP_GT  ||
        t->type == TOKEN_OP_GTGT || t->type == TOKEN_OP_PIPE)
      break;
    if (t->type == TOKEN_WORD)
      argv[i++] = t->value;
    t = t->next;
  }
  argv[i] = NULL;

  *out_argc = word_count;
  return argv;
}

/* ------------------------------------------------------------------ */
/* Redirection helpers for built-in commands.                          */
/* Applies < > >> from the token list to the shell's own fds, using    */
/* the same feeder/tee helpers as external commands (see exec.c), so   */
/* several < files form one stream and every > / >> file gets output.  */
/* Returns 0 on success, -1 on error (caller must not run builtin).    */
/* Caller must call undo_redirections() afterwards.                    */
/* ------------------------------------------------------------------ */
int apply_redirections(Token *start, SavedFds *saved) {
  if (!redirs_validate(start, NULL))
    return -1;
  if (redirs_open(start, NULL, &saved->redirs) != 0)
    return -1;

  fflush(stdout);  /* earlier output must not end up in the files */
  saved->saved_stdin  = dup(STDIN_FILENO);
  saved->saved_stdout = dup(STDOUT_FILENO);
  redirs_install(&saved->redirs);
  return 0;
}

void undo_redirections(const SavedFds *saved) {
  fflush(stdout);
  dup2(saved->saved_stdout, STDOUT_FILENO);
  close(saved->saved_stdout);
  dup2(saved->saved_stdin, STDIN_FILENO);
  close(saved->saved_stdin);
  /* Restoring dropped the shell's pipe ends: a tee now sees EOF, and a
     feeder whose data the built-in never read gets SIGPIPE.  Both exit. */
  redirs_wait(&saved->redirs);
}

/* ------------------------------------------------------------------ */
/* run_group: execute one ;-separated command group starting at start. */
/* Returns 1 if the sequence must stop (a command was not found),      */
/* otherwise 0. Built-ins, pipelines, and external commands handled.   */
/* ------------------------------------------------------------------ */
static int run_group(Token *start, HopEntry *db, int *db_size) {
  if (start == NULL)
    return 0;

  /* Does this group contain a pipe? (scan stops at ; or &) */
  int has_pipe = 0;
  for (Token *t = start; t != NULL; t = t->next) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;
    if (t->type == TOKEN_OP_PIPE) {
      has_pipe = 1;
      break;
    }
  }

  /* ── Built-in commands (only when NOT in a pipeline) ──────────────── */
  if (!has_pipe && start->type == TOKEN_WORD &&
      strcmp(start->value, "hop") == 0) {
    int argc = 0;
    char **argv = extract_group(start, &argc);
    if (argv != NULL) {
      SavedFds saved;
      if (apply_redirections(start, &saved) == 0) {
        hop(argc, argv, db, db_size);
        undo_redirections(&saved);
      }
      free(argv);
      save_hop_db(db, *db_size);
    }
    return 0;
  }

  if (!has_pipe && start->type == TOKEN_WORD &&
      strcmp(start->value, "reveal") == 0) {
    int argc = 0;
    char **argv = extract_group(start, &argc);
    if (argv != NULL) {
      SavedFds saved;
      if (apply_redirections(start, &saved) == 0) {
        reveal(argc, argv);
        undo_redirections(&saved);
      }
      free(argv);
    }
    return 0;
  }

  if (!has_pipe && start->type == TOKEN_WORD &&
      strcmp(start->value, "peek") == 0) {
    int argc = 0;
    char **argv = extract_group(start, &argc);
    if (argv != NULL) {
      SavedFds saved;
      if (apply_redirections(start, &saved) == 0) {
        peek(argc, argv);
        undo_redirections(&saved);
      }
      free(argv);
      clearerr(stdin);
    }
    return 0;
  }

  if (!has_pipe && start->type == TOKEN_WORD &&
      strcmp(start->value, "locate") == 0) {
    int argc = 0;
    char **argv = extract_group(start, &argc);
    if (argv != NULL) {
      SavedFds saved;
      if (apply_redirections(start, &saved) == 0) {
        locate(argc, argv);
        undo_redirections(&saved);
      }
      free(argv);
    }
    return 0;
  }

  if (!has_pipe && start->type == TOKEN_WORD &&
      strcmp(start->value, "resume") == 0) {
    int argc = 0;
    char **argv = extract_group(start, &argc);
    if (argv != NULL) {
      SavedFds saved;
      if (apply_redirections(start, &saved) == 0) {
        resume(argc, argv);
        undo_redirections(&saved);
      }
      free(argv);
    }
    return 0;
  }

  if (!has_pipe && start->type == TOKEN_WORD &&
      strcmp(start->value, "snoop") == 0) {
    int argc = 0;
    char **argv = extract_group(start, &argc);
    if (argv != NULL) {
      SavedFds saved;
      if (apply_redirections(start, &saved) == 0) {
        snoop(argc, argv);
        undo_redirections(&saved);
      }
      free(argv);
    }
    return 0;
  }

  if (!has_pipe && start->type == TOKEN_WORD &&
      strcmp(start->value, "spy") == 0) {
    int argc = 0;
    char **argv = extract_group(start, &argc);
    if (argv != NULL) {
      SavedFds saved;
      if (apply_redirections(start, &saved) == 0) {
        spy(argc, argv);
        undo_redirections(&saved);
      }
      free(argv);
    }
    return 0;
  }

  if (!has_pipe && start->type == TOKEN_WORD &&
      strcmp(start->value, "ping") == 0) {
    int argc = 0;
    char **argv = extract_group(start, &argc);
    if (argv != NULL) {
      SavedFds saved;
      if (apply_redirections(start, &saved) == 0) {
        ping(argc, argv);
        undo_redirections(&saved);
      }
      free(argv);
    }
    return 0;
  }

  if (!has_pipe && start->type == TOKEN_WORD &&
      strcmp(start->value, "activities") == 0) {
    int argc = 0;
    char **argv = extract_group(start, &argc);
    if (argv != NULL) {
      SavedFds saved;
      if (apply_redirections(start, &saved) == 0) {
        activities(argc, argv);
        undo_redirections(&saved);
      }
      free(argv);
    }
    return 0;
  }

  /* ── Pipeline ─────────────────────────────────────────────────────── */
  if (has_pipe)
    return execute_pipeline(start);

  /* ── Single external command ──────────────────────────────────────── */
  int argc = 0;
  char **argv = extract_group(start, &argc);
  if (argv != NULL && argc > 0) {
    int rc = execute_command(argc, argv, start);
    free(argv);
    return rc;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* run_sequence: iterate over every ;/& separated group and run it.   */
/*                                                                      */
/* The operator that FOLLOWS a group determines how it runs:           */
/*   ;  or end-of-input  → foreground (blocking); stop on not-found.  */
/*   &                   → background (non-blocking); always continue. */
/* ------------------------------------------------------------------ */
int run_sequence(TokenList *tokens, HopEntry *db, int *db_size) {
  Token *group = tokens->head;

  while (group != NULL) {
    /* A command group always starts with a WORD token. */
    if (group->type != TOKEN_WORD) {
      group = group->next;
      continue;
    }

    /* ── Find the operator that ends this group ────────────────────── */
    Token *op = group;
    while (op != NULL &&
           op->type != TOKEN_OP_SEMI &&
           op->type != TOKEN_OP_AMP)
      op = op->next;

    if (op != NULL && op->type == TOKEN_OP_AMP) {
      /* ── Background ──────────────────────────────────────────────── */
      run_bg_group(group, db, db_size);
    } else {
      /* ── Foreground (sequential) ─────────────────────────────────── */
      if (run_group(group, db, db_size) != 0)
        break;  /* command not found – stop the sequence */
    }

    /* Advance past the operator (or exit if at end) */
    group = (op != NULL) ? op->next : NULL;
  }
  return 0;
}