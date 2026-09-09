#include "shell.h"

/* ------------------------------------------------------------------ */
/* read_input_line: read a line from stdin.                            */
/*                                                                      */
/* The SIGCHLD handler is installed WITHOUT SA_RESTART, so a           */
/* background process completing interrupts fgets() with EINTR.       */
/* When that happens we report the completion first, then resume.      */
/* This keeps background reporting responsive even while the shell     */
/* is waiting for user input.                                          */
/* Returns 0 on success, 1 when SIGINT cancelled the line, -1 on EOF.  */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t sigint_received = 0;

static void sigint_handler(int sig) {
  (void)sig;
  sigint_received = 1;
}

static int read_input_line(char *input, size_t size) {
  for (;;) {
    errno = 0;
    if (fgets(input, size, stdin) != NULL)
      return 0;

    if (feof(stdin))
      return -1;

    if (errno == EINTR) {
      clearerr(stdin);

      /* Spec #10: report completions as soon as they happen, even while
         waiting for input.  This must run BEFORE the SIGINT check, or a
         ^C arriving around the same time swallows the report. */
      int reported = check_bg_jobs();

      if (sigint_received) {
        sigint_received = 0;
        input[0] = '\0';
        return 1;
      }

      /* A report scrolled the prompt away; draw a fresh one. */
      if (reported > 0)
        display_prompt();
      continue;
    }

    return -1;
  }
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
  init_bg();

  /* Keep the shell alive on Ctrl-C, while allowing the current read to end. */
  struct sigaction sigint_action;
  sigint_action.sa_handler = sigint_handler;
  sigemptyset(&sigint_action.sa_mask);
  sigint_action.sa_flags = 0;
  sigaction(SIGINT, &sigint_action, NULL);
  signal(SIGTSTP, SIG_IGN);

  while (1) {
    display_prompt();

    /* STEP 1: Read user input (wakes on background completion too) */
    int read_status = read_input_line(input, sizeof(input));
    if (read_status < 0) {
      printf("\n");
      break;
    }
    if (read_status > 0) {
      printf("\n");
      continue;
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

    /* STEP 6: Execute the ;-separated sequence (see seq.c) */
    run_sequence(&tokens, db, &db_size);

    /* A ^C during the foreground command set this flag; consume it here.
       Left set, the next SIGCHLD-driven EINTR would be misread as a
       SIGINT and would discard the user's line. */
    if (sigint_received) {
      sigint_received = 0;
      printf("\n");
    }

    free_tokens(&tokens);

    /* Report background processes that finished during this run */
    check_bg_jobs();
  }

  /* Save frecency history on clean exit */
  save_hop_db(db, db_size);

  return 0;
}