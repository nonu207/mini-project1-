#ifndef REVEAL_H
#define REVEAL_H

/*
 * reveal — list directory contents.
 *
 * Syntax: reveal (-(a|t)*)* (~ | . | .. | - | name)?
 *
 *   -a  show hidden files (names starting with '.')
 *   -t  recursively list subdirectory contents after each directory
 *
 * Path resolution follows hop's rules (~, ., .., -, direct path)
 * with no frecency fallback.
 *
 * Error messages:
 *   reveal: no such directory   — path does not resolve
 *   reveal: invalid syntax      — bad flag or too many arguments
 */
void reveal(int argc, char **argv);

#endif /* REVEAL_H */
