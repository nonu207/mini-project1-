#ifndef LOCATE_H
#define LOCATE_H

/*
 * locate — print pathnames of all matching executables.
 *
 * Syntax: locate filename+
 *
 * Searches CWD first, then each entry in PATH, in order.
 * Prints every match found, not just the first.
 */
void locate(int argc, char **argv);

#endif /* LOCATE_H */
