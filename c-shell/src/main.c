#include "shell.h"

/* ------------------------------------------------------------------ */
/* Build a NULL-terminated argv[] from the FIRST command group only.   */
/* Stops at the first TOKEN_OP_SEMI or TOKEN_OP_AMP (or end of list). */
/* Caller must free() the returned pointer.                            */
/* ------------------------------------------------------------------ */
static char **extract_first_group(const TokenList *tokens, int *out_argc) {
  /* Count words in the first command group (before any operator) */
  int word_count = 0;
  Token *t = tokens->head;
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
  t = tokens->head;
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
/* Applies < > >> from the token list to the shell's own fds.          */
/* Returns 0 on success, -1 on error (caller must not run builtin).    */
/* Caller must call undo_redirections() afterwards.                    */
/* ------------------------------------------------------------------ */
typedef struct {
  int saved_stdin;
  int saved_stdout;
} SavedFds;

static int apply_redirections(const TokenList *tokens, SavedFds *saved) {
  saved->saved_stdin  = dup(STDIN_FILENO);
  saved->saved_stdout = dup(STDOUT_FILENO);
  int error = 0;

  Token *t = tokens->head;
  while (t != NULL) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;

    if (t->type == TOKEN_OP_LT) {
      t = t->next;
      if (t == NULL || t->type != TOKEN_WORD) { error = 1; break; }
      int fd = open(t->value, O_RDONLY);
      if (fd < 0) {
        fprintf(stderr, "cshell: no such file or directory\n");
        error = 1;
        break;
      }
      dup2(fd, STDIN_FILENO);
      close(fd);
    } else if (t->type == TOKEN_OP_GT || t->type == TOKEN_OP_GTGT) {
      Token *op = t;
      t = t->next;
      if (t == NULL || t->type != TOKEN_WORD) { error = 1; break; }
      int flags = O_WRONLY | O_CREAT;
      if (op->type == TOKEN_OP_GTGT)
        flags |= O_APPEND;
      else
        flags |= O_TRUNC;
      int fd = open(t->value, flags, 0644);
      if (fd < 0) {
        fprintf(stderr, "cshell: unable to create file for writing\n");
        error = 1;
        break;
      }
      dup2(fd, STDOUT_FILENO);
      close(fd);
    }
    t = t->next;
  }

  if (error) {
    dup2(saved->saved_stdin, STDIN_FILENO);
    dup2(saved->saved_stdout, STDOUT_FILENO);
    close(saved->saved_stdin);
    close(saved->saved_stdout);
    return -1;
  }
  return 0;
}

static void undo_redirections(const SavedFds *saved) {
  fflush(stdout);
  dup2(saved->saved_stdout, STDOUT_FILENO);
  close(saved->saved_stdout);
  dup2(saved->saved_stdin, STDIN_FILENO);
  close(saved->saved_stdin);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(void) {
  char input[1024];
  TokenList tokens;

  /* Load the frecency history once at startup */
  HopEntry db[MAX_HOP_ENTRIES];
  int db_size = 0;
  load_hop_db(db, &db_size);

  init_prompt();

  while (1) {
    display_prompt();

    /* STEP 1: Read user input */
    if (fgets(input, sizeof(input), stdin) == NULL) {
      printf("\n");
      break;
    }

    /* STEP 2: Strip trailing newline */
    input[strcspn(input, "\n")] = '\0';

    /* STEP 3: Skip empty input */
    if (strlen(input) == 0)
      continue;

    /* STEP 4: Tokenize */
    if (tokenize(input, &tokens) != 0)
      continue;
    if (tokens.count == 0)
      continue;

    /* STEP 5: Validate grammar */
    if (validate_grammar(&tokens) != 0) {
      free_tokens(&tokens);
      continue;
    }

    /* ── Helper: check for pipe in current command group ────────────── */
    int first_group_has_pipe = 0;
    for (Token *tp = tokens.head; tp != NULL; tp = tp->next) {
      if (tp->type == TOKEN_OP_SEMI || tp->type == TOKEN_OP_AMP)
        break;
      if (tp->type == TOKEN_OP_PIPE) {
        first_group_has_pipe = 1;
        break;
      }
    }

    /* STEP 6: Dispatch built-in commands (only when NOT in a pipeline) */
    if (!first_group_has_pipe &&
        tokens.head != NULL && tokens.head->type == TOKEN_WORD &&
        strcmp(tokens.head->value, "hop") == 0) {

      int argc = 0;
      char **argv = extract_first_group(&tokens, &argc);

      if (argv != NULL) {
        SavedFds saved;
        if (apply_redirections(&tokens, &saved) == 0) {
          hop(argc, argv, db, &db_size);
          undo_redirections(&saved);
        }
        free(argv);
        save_hop_db(db, db_size);
      }

      free_tokens(&tokens);
      continue;
    }

    /* STEP 7: Dispatch other built-in commands */
    if (!first_group_has_pipe &&
        tokens.head != NULL && tokens.head->type == TOKEN_WORD &&
        strcmp(tokens.head->value, "reveal") == 0) {

      int argc = 0;
      char **argv = extract_first_group(&tokens, &argc);
      if (argv != NULL) {
        SavedFds saved;
        if (apply_redirections(&tokens, &saved) == 0) {
          reveal(argc, argv);
          undo_redirections(&saved);
        }
        free(argv);
      }
      free_tokens(&tokens);
      continue;
    }

    if (!first_group_has_pipe &&
        tokens.head != NULL && tokens.head->type == TOKEN_WORD &&
        strcmp(tokens.head->value, "peek") == 0) {

      int argc = 0;
      char **argv = extract_first_group(&tokens, &argc);
      if (argv != NULL) {
        SavedFds saved;
        if (apply_redirections(&tokens, &saved) == 0) {
          peek(argc, argv);
          undo_redirections(&saved);
        }
        free(argv);
        clearerr(stdin);
      }
      free_tokens(&tokens);
      continue;
    }

    if (!first_group_has_pipe &&
        tokens.head != NULL && tokens.head->type == TOKEN_WORD &&
        strcmp(tokens.head->value, "locate") == 0) {

      int argc = 0;
      char **argv = extract_first_group(&tokens, &argc);
      if (argv != NULL) {
        SavedFds saved;
        if (apply_redirections(&tokens, &saved) == 0) {
          locate(argc, argv);
          undo_redirections(&saved);
        }
        free(argv);
      }
      free_tokens(&tokens);
      continue;
    }

    /* STEP 8: Execute external command or pipeline */
    {
      int has_pipe = 0;
      for (Token *t = tokens.head; t != NULL; t = t->next) {
        if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
          break;
        if (t->type == TOKEN_OP_PIPE) {
          has_pipe = 1;
          break;
        }
      }

      if (has_pipe) {
        execute_pipeline(&tokens);
      } else {
        int argc = 0;
        char **argv = extract_first_group(&tokens, &argc);
        if (argv != NULL && argc > 0) {
          execute_command(argc, argv, &tokens);
          free(argv);
        }
      }
    }

    free_tokens(&tokens);
  }

  /* Save frecency history on clean exit */
  save_hop_db(db, db_size);

  return 0;
}