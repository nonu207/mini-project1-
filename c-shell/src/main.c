#include "shell.h"

/* ------------------------------------------------------------------ */
/* Build a NULL-terminated argv[] array from a TokenList.             */
/* Caller must free() the returned pointer.                            */
/* ------------------------------------------------------------------ */
static char **tokens_to_argv(const TokenList *tokens, int *out_argc) {
  /* Only include TOKEN_WORD tokens as argv entries */
  int word_count = 0;
  Token *t = tokens->head;
  while (t != NULL) {
    if (t->type == TOKEN_WORD)
      word_count++;
    t = t->next;
  }

  /* Allocate space for pointers + NULL sentinel */
  char **argv = malloc((word_count + 1) * sizeof(char *));
  if (argv == NULL)
    return NULL;

  int i = 0;
  t = tokens->head;
  while (t != NULL) {
    if (t->type == TOKEN_WORD)
      argv[i++] = t->value;
    t = t->next;
  }
  argv[i] = NULL;

  *out_argc = word_count;
  return argv;
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

    /* STEP 6: Dispatch built-in commands */
    if (tokens.head != NULL && tokens.head->type == TOKEN_WORD &&
        strcmp(tokens.head->value, "hop") == 0) {

      /* Build argv from token list */
      int argc = 0;
      char **argv = tokens_to_argv(&tokens, &argc);

      if (argv != NULL) {
        hop(argc, argv, db, &db_size);
        free(argv);

        /* Persist the updated frecency database */
        save_hop_db(db, db_size);
      }

      free_tokens(&tokens);
      continue;
    }

    /* STEP 7: Dispatch other built-in commands */
    if (tokens.head != NULL && tokens.head->type == TOKEN_WORD &&
        strcmp(tokens.head->value, "reveal") == 0) {

      int argc = 0;
      char **argv = tokens_to_argv(&tokens, &argc);
      if (argv != NULL) {
        reveal(argc, argv);
        free(argv);
      }
      free_tokens(&tokens);
      continue;
    }

    if (tokens.head != NULL && tokens.head->type == TOKEN_WORD &&
        strcmp(tokens.head->value, "peek") == 0) {

      int argc = 0;
      char **argv = tokens_to_argv(&tokens, &argc);
      if (argv != NULL) {
        peek(argc, argv);
        free(argv);

        /* If peek consumed stdin until EOF, clear the EOF flag
         * so the shell's main loop doesn't immediately exit. */
        clearerr(stdin);
      }
      free_tokens(&tokens);
      continue;
    }

    if (tokens.head != NULL && tokens.head->type == TOKEN_WORD &&
        strcmp(tokens.head->value, "locate") == 0) {

      int argc = 0;
      char **argv = tokens_to_argv(&tokens, &argc);
      if (argv != NULL) {
        locate(argc, argv);
        free(argv);
      }
      free_tokens(&tokens);
      continue;
    }

    /* print_tokens(&tokens); */

    /* STEP 8: Clean up */
    free_tokens(&tokens);
  }

  /* Save frecency history on clean exit */
  save_hop_db(db, db_size);

  return 0;
}