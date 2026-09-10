#include "shell.h"

static pid_t shell_pgid = 0;
static int   have_tty   = 0;

void term_init(void) {
  have_tty = isatty(STDIN_FILENO);

  /* Spec: SIGTTOU must not stop the shell.  tcsetpgrp() from a process
     that is not in the foreground group raises SIGTTOU, whose default
     action is to stop the caller -- which would freeze the shell every
     time it reclaimed the terminal.  Ignoring it makes the call succeed. */
  signal(SIGTTOU, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);

  if (!have_tty)
    return;

  /* Put the shell in its own process group and take the terminal, so the
     shell is never a member of a job's group. */
  shell_pgid = getpid();
  if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EPERM) {
    /* EPERM means we are already a session leader: that is fine. */
    shell_pgid = getpgrp();
  }
  tcsetpgrp(STDIN_FILENO, shell_pgid);
}

void term_give(pid_t pgid) {
  if (have_tty && pgid > 0)
    tcsetpgrp(STDIN_FILENO, pgid);
}

void term_take(void) {
  if (have_tty)
    tcsetpgrp(STDIN_FILENO, shell_pgid);
}

int term_is_tty(void) {
  return have_tty;
}
