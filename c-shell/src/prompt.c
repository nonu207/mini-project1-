#include "shell.h"

/* The directory the shell was started in, treated as its home directory. */
static char shell_home[1024] = "";

const char *get_shell_home(void) { return shell_home; }

/* Records the shell's startup directory as its home directory, used
 * later to shorten the prompt with a leading tilde. */
void init_prompt(void) {
    if (getcwd(shell_home, sizeof(shell_home)) == NULL)
        shell_home[0] = '\0';
}

/* Prints the shell prompt in the form <user@host:path>. The path is
 * shown as a tilde when the current directory is exactly the shell's
 * home directory, as a tilde followed by a relative path when it is
 * inside the home directory, and as an absolute path otherwise. The
 * hostname is truncated at its first dot, so a name such as
 * MacBook-Pro.local is shown as MacBook-Pro. */
void display_prompt(void) {
    char cwd[1024];
    char hostname[256];
    char username[256];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd");
        snprintf(cwd, sizeof(cwd), "?");
    }

    if (gethostname(hostname, sizeof(hostname)) == -1) {
        perror("gethostname");
        snprintf(hostname, sizeof(hostname), "?");
    }

    struct passwd *pw = getpwuid(getuid());
    if (pw != NULL) {
        snprintf(username, sizeof(username), "%s", pw->pw_name);
    } else {
        snprintf(username, sizeof(username), "uid%d", getuid());
    }

    char display_path[1024];
    size_t home_len = strlen(shell_home);
    if (shell_home[0] != '\0' && strcmp(cwd, shell_home) == 0) {
        snprintf(display_path, sizeof(display_path), "~");
    } else if (shell_home[0] != '\0' && home_len > 0
               && strncmp(cwd, shell_home, home_len) == 0
               && cwd[home_len] == '/') {
        snprintf(display_path, sizeof(display_path), "~%s", cwd + home_len);
    } else {
        snprintf(display_path, sizeof(display_path), "%s", cwd);
    }

    char *dot = strchr(hostname, '.');
    if (dot != NULL) *dot = '\0';

    printf("<%s@%s:%s> ", username, hostname, display_path);
    fflush(stdout);
}
