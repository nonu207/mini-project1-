#ifndef EXEC_H
#define EXEC_H

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
 * @param tokens Full token list (used to detect < > redirections)
 */
void execute_command(int argc, char **argv, const TokenList *tokens);

/**
 * Execute a pipeline of commands connected by pipes.
 * Handles per-command < > redirections as well.
 *
 * @param tokens Full token list (stops at first ; or & or end)
 */
void execute_pipeline(const TokenList *tokens);

#endif /* EXEC_H */
