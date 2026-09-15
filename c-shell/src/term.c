#include "shell.h"

static pid_t shell_pgid = 0;
static int   have_tty   = 0;

/* Sets up terminal control at shell startup. Checks whether stdin is a
 * real terminal. SIGTTOU is ignored because tcsetpgrp() from a process
 * that is not in the foreground group raises SIGTTOU, and its default
 * action would stop the shell every time it reclaimed the terminal.
 * SIGTTIN is ignored for the same reason. If there is no terminal the
 * function returns early, since none of the rest matters. Otherwise the
 * shell puts itself in its own process group, falling back to whatever
 * group it is already in if it turns out to be a session leader, and
 * takes the terminal for that group, so the shell is never a member of
 * a job's process group. */
void term_init(void) {
  have_tty = isatty(STDIN_FILENO);

  signal(SIGTTOU, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);

  if (!have_tty)
    return;

  shell_pgid = getpid();
  if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EPERM) {
    /* EPERM means the shell is already a session leader, which is fine. */
    shell_pgid = getpgrp();
  }
  tcsetpgrp(STDIN_FILENO, shell_pgid);
}

/* Hands the terminal to a job's process group so that job can read from
 * and be interrupted by the terminal. Does nothing if the shell has no
 * terminal or the group id is invalid. */
void term_give(pid_t pgid) {
  if (have_tty && pgid > 0)
    tcsetpgrp(STDIN_FILENO, pgid);
}

/* Takes the terminal back for the shell's own process group, once a
 * foreground job has finished or stopped. */
void term_take(void) {
  if (have_tty)
    tcsetpgrp(STDIN_FILENO, shell_pgid);
}

/* Reports whether the shell is attached to a real terminal. */
int term_is_tty(void) {
  return have_tty;
}
