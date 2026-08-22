#ifndef PROMPT_H
#define PROMPT_H

#include <stddef.h>

void init_prompt(void);
void display_prompt(void);
void format_relative_path(const char *full_path, char *out_buf, size_t buf_size);

/* Returns the directory the shell was started in (the shell's "~"). */
const char *get_shell_home(void);

#endif
