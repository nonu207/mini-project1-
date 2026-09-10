#ifndef TERM_H
#define TERM_H

#include <sys/types.h>

/*
 * Terminal / job-control ownership.
 *
 * Only ONE process group at a time may read from the terminal: the
 * foreground group, as set by tcsetpgrp().  The kernel sends terminal
 * signals (^C -> SIGINT, ^Z -> SIGTSTP) to exactly that group, which is
 * what keeps background jobs insulated from the keyboard.
 *
 * The shell claims the terminal at startup, hands it to each foreground
 * pipeline, and takes it back when that pipeline exits or stops.
 */

/* Record the shell's own pgid, ignore SIGTTOU, and claim the terminal.
   Safe to call when stdin is not a tty; everything then becomes a no-op. */
void term_init(void);

/* Hand the terminal to a foreground job's process group. */
void term_give(pid_t pgid);

/* Reclaim the terminal for the shell. */
void term_take(void);

/* Is the shell attached to a terminal at all? */
int  term_is_tty(void);

#endif /* TERM_H */
