#include "prompt.h"
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Initialize prompt (reserved for future use)
void init_prompt(void) {
    // Currently no initialization needed
}

void display_prompt(void) {
    char cwd[1024];
    char hostname[256];
    char username[256];
    char home[1024];

    getcwd(home, sizeof(home)); 
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

    // Get actual username and home directory from password database
    struct passwd *pw = getpwuid(getuid());
    if (pw != NULL) {
        snprintf(username, sizeof(username), "%s", pw->pw_name);
    } else {
        snprintf(username, sizeof(username), "uid%d", getuid());
        home[0] = '\0';
    }

    // Convert to relative path using home directory for cleaner display
    char display_path[1024];
    if (home[0] != '\0' && strcmp(cwd, home) == 0) {
        // User is in home directory
        snprintf(display_path, sizeof(display_path), "~");
    } else if (home[0] != '\0' && strncmp(cwd, home, strlen(home)) == 0 && 
               cwd[strlen(home)] == '/') {
        // User is in a subdirectory of home - display as ~/subdir
        snprintf(display_path, sizeof(display_path), "~%s", cwd + strlen(home));
    } else {
        // Outside home directory or home not available - show full path
        snprintf(display_path, sizeof(display_path), "%s", cwd);
    }

    printf("%s@%s:%s$ ", username, hostname, display_path);
    fflush(stdout);
}