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
 * @param start  First token of the command group (scans up to ; or & or end)
 * @return  PID of the FIRST command in the pipeline (as required by the spec),
 *          or 0 on error.
 */
pid_t execute_pipeline_bg(Token *start);

#endif /* EXEC_H */
