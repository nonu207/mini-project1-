#ifndef PEEK_H
#define PEEK_H

/*
 * peek — read and display files with optional numbering and reversing.
 *
 * Syntax: peek (-(n|r)*)* filename*
 *
 *   -n  number non-empty lines
 *   -r  reverse line order
 */
void peek(int argc, char **argv);

#endif /* PEEK_H */
