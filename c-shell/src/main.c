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
static volatile sig_atomic_t sigint_received  = 0;
static volatile sig_atomic_t sigtstp_received = 0;

static void sigint_handler(int sig) {
  (void)sig;
  sigint_received = 1;
}

/* Spec: the shell itself must never be stopped by SIGTSTP.  Installing a
   handler at all is what prevents the default stop; the flag just lets
   the main loop cancel the current line and redraw the prompt. */
static void sigtstp_handler(int sig) {
  (void)sig;
  sigtstp_received = 1;
}

/* ------------------------------------------------------------------ */
/* read_input_line: read one line from stdin.                          */
/*                                                                      */
/* Returns 0 on success, 1 when SIGINT/SIGTSTP cancelled the line, and  */
/* -1 on EOF (Ctrl-D) -- but ONLY when the line is empty.               */
/*                                                                      */
/* Ctrl-D mid-line does not end the file: the terminal simply flushes   */
/* what has been typed so far, so fgets returns text with no trailing   */
/* newline and feof() stays clear.  We keep that text in the buffer and */
/* loop, which is how the spec's "keep the text and stay alive" falls   */
/* out naturally.                                                       */
/* ------------------------------------------------------------------ */
static int read_input_line(char *input, size_t size) {
  size_t len = 0;
  input[0] = '\0';

  for (;;) {
    errno = 0;

    if (fgets(input + len, (int)(size - len), stdin) != NULL) {
      len += strlen(input + len);

      if (len > 0 && input[len - 1] == '\n')
        return 0;                       /* complete line */
      if (len + 1 >= size)
        return 0;                       /* buffer full; run what we have */
      if (feof(stdin)) {
        clearerr(stdin);
        return (len == 0) ? -1 : 0;
      }
      continue;                         /* partial line: keep the text */
    }

    if (feof(stdin)) {
      clearerr(stdin);
      /* Ctrl-D counts as EOF only on an empty line. */
      return (len == 0) ? -1 : 0;
    }

    if (errno == EINTR) {
      clearerr(stdin);

      /* Spec: report completions as soon as they happen, even while
         waiting for input.  This must run BEFORE the signal checks, or a
         ^C arriving around the same time swallows the report. */
      int reported = check_bg_jobs();

      if (sigint_received || sigtstp_received) {
        sigint_received = sigtstp_received = 0;
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
  /* Claim the terminal and make SIGTTOU harmless before any job runs. */
  term_init();

  /* Keep the shell alive on Ctrl-C and Ctrl-Z, while allowing the current
     read to end.  No SA_RESTART: the interrupted fgets is what lets the
     shell redraw its prompt. */
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sa.sa_handler = sigint_handler;
  sigaction(SIGINT, &sa, NULL);

  sa.sa_handler = sigtstp_handler;
  sigaction(SIGTSTP, &sa, NULL);

  /* Ctrl-D on an empty line warns once while jobs are stopped; a second
     Ctrl-D with no input in between exits anyway. */
  int eof_pending = 0;

  while (1) {
    display_prompt();

    /* STEP 1: Read user input (wakes on background completion too) */
    int read_status = read_input_line(input, sizeof(input));
    if (read_status < 0) {
      printf("\n");
      if (!eof_pending && bg_has_stopped()) {
        fprintf(stderr, "cshell: there are stopped jobs\n");
        eof_pending = 1;
        continue;
      }
      break;
    }
    /* Any other input clears the "second Ctrl-D exits" arming. */
    eof_pending = 0;
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
    if (sigint_received || sigtstp_received) {
      sigint_received = sigtstp_received = 0;
      printf("\n");
    }

    free_tokens(&tokens);

    /* Report background processes that finished during this run */
    check_bg_jobs();
  }

  /* Spec: hang up every tracked job's process group before exiting, and
     do not wait for them. */
  bg_hangup_all();

  /* Save frecency history on clean exit */
  save_hop_db(db, db_size);

  return 0;
}