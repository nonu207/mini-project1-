#include "shell.h"

/* Builds a NULL-terminated argv array from one command group, stopping
 * at the first operator (semicolon, ampersand, redirection, or pipe) or
 * the end of the token list. The caller must free the returned array. */
static char **extract_group(Token *start, int *out_argc) {
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

/* Applies a builtin's redirections, using the same feeder and tee
 * helpers external commands use, so several input files are joined
 * into one stream and every output file receives the full output.
 * Returns 0 on success, in which case the caller must eventually call
 * undo_redirections, or -1 on error, in which case the builtin must not
 * be run. */
int apply_redirections(Token *start, SavedFds *saved) {
  if (!redirs_validate(start, NULL))
    return -1;
  if (redirs_open(start, NULL, &saved->redirs) != 0)
    return -1;

  /* Output already produced must not end up inside the redirected
   * files, so it is flushed first. */
  fflush(stdout);
  saved->saved_stdin  = dup(STDIN_FILENO);
  saved->saved_stdout = dup(STDOUT_FILENO);
  redirs_install(&saved->redirs);
  return 0;
}

/* Restores the shell's own stdin and stdout after a builtin ran with
 * redirections applied. Once the shell's pipe ends are closed here, any
 * tee helper sees end of file and any feeder whose data the builtin
 * never read receives SIGPIPE, so both exit on their own; this waits
 * for that to happen. */
void undo_redirections(const SavedFds *saved) {
  fflush(stdout);
  dup2(saved->saved_stdout, STDOUT_FILENO);
  close(saved->saved_stdout);
  dup2(saved->saved_stdin, STDIN_FILENO);
  close(saved->saved_stdin);
  redirs_wait(&saved->redirs);
}

/* Runs one command group, which is everything up to the next semicolon
 * or ampersand. Dispatches to a builtin, a pipeline, or a single
 * external command as appropriate. Returns 1 if the sequence must stop
 * because a command was not found, otherwise 0. */
static int run_group(Token *start, HopEntry *db, int *db_size) {
  if (start == NULL)
    return 0;

  int has_pipe = 0;
  for (Token *t = start; t != NULL; t = t->next) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;
    if (t->type == TOKEN_OP_PIPE) {
      has_pipe = 1;
      break;
    }
  }

  /* Each builtin below is only recognized when the group has no pipe,
   * since a builtin inside a pipeline is instead handled as one of the
   * pipeline's own stages. */

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

  if (has_pipe)
    return execute_pipeline(start);

  int argc = 0;
  char **argv = extract_group(start, &argc);
  if (argv != NULL && argc > 0) {
    int rc = execute_command(argc, argv, start);
    free(argv);
    return rc;
  }
  return 0;
}

/* Runs every semicolon- and ampersand-separated group in a tokenized
 * line, in order. The operator that follows a group decides how it
 * runs: a semicolon or the end of input runs it in the foreground and
 * blocks until it finishes, stopping the whole sequence if the command
 * was not found; an ampersand runs it in the background and always
 * continues on to the next group regardless of the outcome. */
int run_sequence(TokenList *tokens, HopEntry *db, int *db_size) {
  Token *group = tokens->head;

  while (group != NULL) {
    if (group->type != TOKEN_WORD) {
      group = group->next;
      continue;
    }

    Token *op = group;
    while (op != NULL &&
           op->type != TOKEN_OP_SEMI &&
           op->type != TOKEN_OP_AMP)
      op = op->next;

    if (op != NULL && op->type == TOKEN_OP_AMP) {
      run_bg_group(group, db, db_size);
    } else {
      if (run_group(group, db, db_size) != 0)
        break;
    }

    group = (op != NULL) ? op->next : NULL;
  }
  return 0;
}
