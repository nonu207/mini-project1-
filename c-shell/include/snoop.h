#ifndef SNOOP_H
#define SNOOP_H

/*
 * snoop — trace a process's system calls and summarise them when it exits.
 *
 * Syntax: snoop command [args...]
 *         snoop -p pid
 *
 * The first form forks, calls PTRACE_TRACEME in the child and execs the
 * command; the second attaches to a running process with PTRACE_ATTACH.
 * Either way the tracee is stepped with PTRACE_SYSCALL, stopping at every
 * syscall entry and exit, and the time between the two is accumulated.
 *
 * When the tracee exits, prints:
 *
 *   syscall       calls   time
 *   nanosleep     1       1.000s
 *
 * sorted by call count (descending), ties by first occurrence.  Numbers
 * missing from the lookup table print as syscall_N.
 *
 * With -p, Ctrl-C detaches (leaving the process running) and prints the
 * summary so far.
 *
 * Errors: "snoop: no such process", "snoop: command not found",
 *         "snoop: permission denied", "snoop: invalid syntax".
 *
 * Linux only; elsewhere prints "snoop: not supported on this system".
 */
void snoop(int argc, char **argv);

#endif /* SNOOP_H */
