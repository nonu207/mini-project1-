#include "shell.h"

static volatile sig_atomic_t sigint_received  = 0;
static volatile sig_atomic_t sigtstp_received = 0;

/* Records that SIGINT arrived. The handler is installed without
 * SA_RESTART, so it interrupts a blocking fgets with EINTR, which lets
 * the main loop notice and cancel the current line. */
static void sigint_handler(int sig) {
  (void)sig;
  sigint_received = 1;
}

/* Records that SIGTSTP arrived. Installing a handler at all is what
 * keeps the shell from being stopped by the default action; the flag
 * just lets the main loop cancel the current line and redraw the
 * prompt afterward. */
static void sigtstp_handler(int sig) {
  (void)sig;
  sigtstp_received = 1;
}

/* Reads one line of input from stdin. Returns 0 on a complete line, 1
 * if SIGINT or SIGTSTP cancelled the read, and -1 on end of file, which
 * only counts when the line read so far is empty.
 *
 * Ctrl-D in the middle of a line does not end the file. The terminal
 * simply flushes what has been typed so far, so fgets returns that text
 * with no trailing newline and feof stays clear. On a real terminal
 * that text is kept and the read continues until Enter, which is how
 * the requirement to keep the text and stay alive is satisfied. When
 * input is not a terminal, a final line with no trailing newline is
 * still run as is.
 *
 * While waiting, an interrupting SIGCHLD is used to report any
 * background job that has just finished, before checking whether the
 * interrupt was actually a Ctrl-C or Ctrl-Z, so a completion is never
 * swallowed by a signal that arrives at the same moment. */
static int read_input_line(char *input, size_t size) {
  size_t len = 0;
  input[0] = '\0';

  for (;;) {
    errno = 0;

    if (fgets(input + len, (int)(size - len), stdin) != NULL) {
      len += strlen(input + len);

      if (len > 0 && input[len - 1] == '\n')
        return 0;
      if (len + 1 >= size)
        return 0;
      if (feof(stdin)) {
        clearerr(stdin);
        if (len == 0)
          return -1;
        if (term_is_tty())
          continue;
        return 0;
      }
      continue;
    }

    if (feof(stdin)) {
      clearerr(stdin);
      if (len == 0)
        return -1;
      if (term_is_tty())
        continue;
      return 0;
    }

    if (errno == EINTR) {
      clearerr(stdin);

      int reported = check_bg_jobs();

      if (sigint_received || sigtstp_received) {
        sigint_received = sigtstp_received = 0;
        input[0] = '\0';
        return 1;
      }

      if (reported > 0)
        display_prompt();
      continue;
    }

    return -1;
  }
}

/* Entry point. Sets up the frecency history, the prompt, background job
 * tracking, terminal control and signal handling, then runs the
 * read, tokenize and execute loop until end of file. On exit, every
 * tracked job is hung up and the frecency history is saved. */
int main(void) {
  char input[1024];
  TokenList tokens;

  HopEntry db[MAX_HOP_ENTRIES];
  int db_size = 0;
  load_hop_db(db, &db_size);

  init_prompt();
  init_bg();
  spy_init();
  term_init();

  /* SIGINT and SIGTSTP are handled, not ignored, so the shell survives
   * them but the interrupted read still returns and lets the main loop
   * redraw the prompt. SA_RESTART is deliberately left off. */
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sa.sa_handler = sigint_handler;
  sigaction(SIGINT, &sa, NULL);

  sa.sa_handler = sigtstp_handler;
  sigaction(SIGTSTP, &sa, NULL);

  /* Ctrl-D on an empty line only warns once while jobs are stopped; a
   * second Ctrl-D with no other input in between exits anyway. */
  int eof_pending = 0;

  while (1) {
    display_prompt();

    int read_status = read_input_line(input, sizeof(input));
    if (read_status < 0) {
      printf("\n");
      /* Applies any stop the SIGCHLD handler has queued but not yet
       * reported, so a job stopped from outside, such as kill -STOP or
       * a background job reading the terminal, also counts here. */
      check_bg_jobs();
      if (!eof_pending && bg_has_stopped()) {
        fprintf(stderr, "cshell: there are stopped jobs\n");
        eof_pending = 1;
        continue;
      }
      break;
    }
    eof_pending = 0;
    if (read_status > 0) {
      printf("\n");
      continue;
    }

    input[strcspn(input, "\n")] = '\0';

    if (strlen(input) == 0)
      continue;

    if (tokenize(input, &tokens) != 0)
      continue;
    if (tokens.count == 0)
      continue;

    if (validate_grammar(&tokens) != 0) {
      free_tokens(&tokens);
      continue;
    }

    run_sequence(&tokens, db, &db_size);

    /* A Ctrl-C during the foreground command set this flag. It is
     * consumed here so that the next SIGCHLD-driven interrupt is not
     * mistaken for a fresh Ctrl-C, which would otherwise discard the
     * next line the user types. */
    if (sigint_received || sigtstp_received) {
      sigint_received = sigtstp_received = 0;
      printf("\n");
    }

    free_tokens(&tokens);

    check_bg_jobs();
  }

  bg_hangup_all();

  save_hop_db(db, db_size);

  return 0;
}
