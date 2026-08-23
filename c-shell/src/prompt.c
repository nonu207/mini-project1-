#include "shell.h"

/* The directory where the shell was started — this is the shell's "home". */
static char shell_home[1024] = "";

const char *get_shell_home(void) { return shell_home; }

// Initialize prompt: capture the startup directory as the shell's home.
void init_prompt(void) {
    if (getcwd(shell_home, sizeof(shell_home)) == NULL)
        shell_home[0] = '\0';
}

void display_prompt(void) {
    char cwd[1024];
    char hostname[256];
    char username[256];

    // Get current working directory with error handling
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd");
        snprintf(cwd, sizeof(cwd), "?");
    }

    // Get hostname with error handling
    if (gethostname(hostname, sizeof(hostname)) == -1) {
        perror("gethostname");
        snprintf(hostname, sizeof(hostname), "?");
    }

    // Get actual username from password database
    struct passwd *pw = getpwuid(getuid());
    if (pw != NULL) {
        snprintf(username, sizeof(username), "%s", pw->pw_name);
    } else {
        snprintf(username, sizeof(username), "uid%d", getuid());
    }

    // Convert to relative path using shell_home for cleaner display
    char display_path[1024];
    size_t home_len = strlen(shell_home);
    if (shell_home[0] != '\0' && strcmp(cwd, shell_home) == 0) {
        // Exactly in the shell's home directory
        snprintf(display_path, sizeof(display_path), "~");
    } else if (shell_home[0] != '\0' && home_len > 0
               && strncmp(cwd, shell_home, home_len) == 0
               && cwd[home_len] == '/') {
        // In a subdirectory of shell home — show as ~/subdir
        snprintf(display_path, sizeof(display_path), "~%s", cwd + home_len);
    } else {
        // Outside shell home — show absolute path as-is
        snprintf(display_path, sizeof(display_path), "%s", cwd);
    }

    /* Truncate hostname at the first '.' for a cleaner display
     * e.g. "MacBook-Pro.local" -> "MacBook-Pro" */
    char *dot = strchr(hostname, '.');
    if (dot != NULL) *dot = '\0';

    printf("<%s@%s:%s> ", username, hostname, display_path);
    fflush(stdout);
}