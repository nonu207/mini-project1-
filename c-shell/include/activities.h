#ifndef ACTIVITIES_H
#define ACTIVITIES_H

/*
 * activities — list every process the shell has spawned that is still
 * running, grouped by process group.
 *
 * Syntax: activities
 *
 * Prints one line per process group, oldest group first, followed by one
 * indented line per live process in that group:
 *
 *   [job_number] pgid pgid_value
 *     pid command_name state
 *
 * where state is "Running" or "Stopped".  Processes that have already
 * exited are reaped and removed before anything is printed.
 *
 * Takes no arguments; anything else is "activities: invalid syntax".
 */
void activities(int argc, char **argv);

#endif /* ACTIVITIES_H */
