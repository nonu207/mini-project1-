#ifndef EXEC_H
#define EXEC_H

#include <sys/types.h>

/**
 * Resolve a command name to an executable path.
 *
 * Resolution rules:
 *   1. If name contains '/', treat as literal path.
 *   2. If name starts with '%', skip CWD check, search PATH directly.
 *   3. Otherwise, check CWD first, then search PATH.
 *
 * @param name  Command name to resolve
 * @return  Heap-allocated full path string (caller must free), or NULL
 */
char *resolve_command(const char *name);

/**
 * The redirections of one command, opened and ready to install.
 *
 *   in_fd / out_fd  become the command's stdin / stdout (-1: unchanged).
 *   helper[0]       feeder process that joins several < files, in order,
 *                   into one stream (0 when unused)
 *   helper[1]       tee process that copies output into several > / >>
 *                   files, each with its own mode (0 when unused)
 *
 * A single < or > is a plain file descriptor with no helper.
 */
typedef struct {
  int   in_fd;
  int   out_fd;
  pid_t helper[2];
} Redirs;

/**
 * Check every redirection target in [start, end) (stopping early at ; or &).
 * Prints the spec's error message and returns 0 on the first failure,
 * 1 if all targets can be opened.  > files are created/truncated here.
 */
int redirs_validate(Token *start, Token *end);

/**
 * Open the redirections of [start, end), forking a feeder for multiple <
 * and a tee for multiple > / >>.  Call after redirs_validate.
 * Returns 0 on success, -1 on failure (nothing is left open).
 */
int redirs_open(Token *start, Token *end, Redirs *r);

/** dup2 the opened fds onto stdin/stdout and close the originals. */
void redirs_install(Redirs *r);

/** Close the opened fds without installing them (e.g. parent after fork). */
void redirs_close(Redirs *r);

/**
 * Reap the feeder/tee helpers.  Call only once every holder of the
 * redirection fds has closed them, or the tee never sees EOF.
 */
void redirs_wait(const Redirs *r);

/**
 * Execute a single external command via fork/exec, with redirection support.
 *
 * @param argc   Argument count (argv[0] is the command name)
 * @param argv   NULL-terminated argument vector
 * @param start  First token of this command group (scans up to ; or & or end)
 * @return  0 on success; 1 if the command was not found
 */
int execute_command(int argc, char **argv, Token *start);

/**
 * Execute a pipeline of commands connected by pipes.
 * Handles per-command < > redirections as well.
 *
 * A stage whose command is not found prints its error, and the rest of the
 * pipeline still runs.  Per spec C4 this is not a failed command, so it
 * never stops a ; sequence.
 *
 * @param start  First token of this command group (scans up to ; or & or end)
 * @return  0 always
 */
int execute_pipeline(Token *start);

/**
 * Execute a pipeline in the background (non-blocking).
 *
 * Forks all pipeline stages and sets up their inter-process pipes, then
 * returns WITHOUT waiting for any child to finish.  The stages never own
 * the terminal: on a tty a terminal read stops them with SIGTTIN, and
 * without a tty their stdin is /dev/null (see bg_child_setup).
 *
 * Every stage is placed in ONE process group headed by the first stage, so
 * that terminal-generated signals never reach it and so that activities can
 * report the pipeline as a single group.
 *
 * Only successfully forked stages are reported back, in pipeline order, so
 * out_pids[0] is always the first live stage and equals the returned pgid.
 *
 * @param start      First token of the command group (scans up to ; or & or end)
 * @param out_pids   Caller array, filled with the pid of each stage
 * @param out_names  Caller array, filled with argv[0] of each stage.  The
 *                   names are COPIED because argv[0] points into Token
 *                   storage that free_tokens() releases after this returns.
 * @param out_cap    Capacity of out_pids / out_names
 * @param out_count  Set to the number of entries actually written
 * @return  PGID of the pipeline == PID of the FIRST command (as required by
 *          the spec), or 0 on error.
 */
pid_t execute_pipeline_bg(Token *start, pid_t *out_pids,
                          char (*out_names)[256],
                          int out_cap, int *out_count);

#endif /* EXEC_H */
