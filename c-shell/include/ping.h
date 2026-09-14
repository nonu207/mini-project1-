#ifndef PING_H
#define PING_H

/*
 * ping — send a signal to a tracked process or to a whole job.
 *
 * Syntax: ping <target> <signal_number>
 *
 * <target> is a pid, or %job_number (the number activities prints), in
 * which case the signal goes to every process in that job's group.  Only
 * pids and jobs the shell spawned and is still tracking are accepted.
 *
 * The signal actually sent is signal_number % 64, but the message echoes
 * the number exactly as typed:
 *
 *   Sent signal <signal_number> to <target>
 *
 * signal_number is validated before <target> is looked up.
 *
 * Errors: "ping: invalid syntax"          (signal_number not a
 *                                          non-negative integer)
 *         "ping: no such process found"   (target unknown)
 */
void ping(int argc, char **argv);

#endif /* PING_H */
