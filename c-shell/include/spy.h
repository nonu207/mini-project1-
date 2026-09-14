#ifndef SPY_H
#define SPY_H

/*
 * spy — list the open files of a process, lsof-style, from /proc.
 *
 * Syntax: spy [pid]
 *
 * With no pid, reports the shell itself.  Prints a header, then one row
 * per open object:
 *
 *   PID    FD    TYPE   PATH
 *
 * in this order: cwd (working directory), txt (the executable), one mem
 * row per unique memory-mapped file, then numeric descriptors ascending.
 * TYPE is REG, DIR, CHR, BLK, FIFO, LINK, SOCK, or a_inode.
 *
 * Errors: "spy: invalid syntax"   (more than one argument, or a pid that
 *                                  is not a non-negative integer)
 *         "spy: no such process"  (no /proc entry for that pid)
 */
void spy(int argc, char **argv);

/* Record the shell's pid.  Call once from main, before any fork, so a
   bare "spy" inside a pipeline or background job still reports the shell
   rather than the forked child it happens to be running in. */
void spy_init(void);

#endif /* SPY_H */
