#ifndef RESUME_H
#define RESUME_H

/*
 * resume — continue a stopped or backgrounded job.
 *
 * Syntax: resume %job_number (fg [--timeout <seconds>] | bg)
 *
 * job_number is the number activities prints.  The job's process group is
 * sent SIGCONT either way; the two modes differ in who owns the terminal:
 *
 *   fg  gives the job the terminal, waits for it to finish or stop again,
 *       then reclaims the terminal.  With --timeout, a timer is armed
 *       before the wait; if it fires first the job is sent SIGTERM,
 *       "resume: job timed out" is printed, and the job is dropped.
 *   bg  marks the job Running and returns to the prompt immediately,
 *       never touching the terminal.
 *
 * Errors: "resume: no such job", "resume: invalid syntax".
 */
void resume(int argc, char **argv);

#endif /* RESUME_H */
