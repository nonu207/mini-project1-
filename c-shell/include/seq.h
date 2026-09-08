#ifndef SEQ_H
#define SEQ_H

/**
 * Execute a list of `;`-separated command groups in strict order.
 *
 * Each command group (a complete shell_cmd as defined by the grammar) is
 * executed one after another: the shell waits for each group to finish
 * before starting the next. If a group fails to execute because its
 * command was not found, `cshell: command not found (name)` is printed
 * and the remaining groups in the sequence are skipped.
 *
 * The prompt is displayed either after every group completed or as soon
 * as the sequence stopped due to a failure.
 *
 * @param tokens    Tokenized + grammar-validated input line
 * @param db        Frecency history (used by the `hop` built-in)
 * @param db_size   Pointer to the current size of `db`
 * @return  0 always (the stop-on-failure decision is internal)
 */
int run_sequence(TokenList *tokens, HopEntry *db, int *db_size);

/* Redirection helpers shared with bg.c (used by built-in commands). */
typedef struct {
  int saved_stdin;
  int saved_stdout;
} SavedFds;

int  apply_redirections(Token *start, SavedFds *saved);
void undo_redirections(const SavedFds *saved);

#endif /* SEQ_H */