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
 * @param start  First token of this command group (scans up to ; or & or end)
 * @return  0 if every pipeline stage was found; 1 otherwise
 */
int execute_pipeline(Token *start);

/**
 * Execute a pipeline in the background (non-blocking).
 *
 * Forks all pipeline stages and sets up their inter-process pipes, then
 * returns WITHOUT waiting for any child to finish.  stdin of every child
 * in the pipeline is redirected to /dev/null so the pipeline has no
 * access to the terminal.
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
