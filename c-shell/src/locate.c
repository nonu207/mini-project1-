#include "shell.h"

/* ------------------------------------------------------------------ */
/* check_exec: checks if `path` is an executable regular file.        */
/* Returns 1 if it is, 0 otherwise.                                   */
/* ------------------------------------------------------------------ */
static int check_exec(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)      return 0; /* doesn't exist          */
    if (S_ISDIR(st.st_mode))       return 0; /* skip directories       */
    if (access(path, X_OK) != 0)   return 0; /* not executable         */
    return 1;
}

/* ------------------------------------------------------------------ */
/* locate_one: search for a single filename and print all matches.    */
/* Returns 1 if at least one match was found, 0 if none.              */
/* ------------------------------------------------------------------ */
static int locate_one(const char *name) {
    int found = 0;
    char fullpath[PATH_MAX];

    /* ── 1. Check the current working directory first ──────────────── */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        snprintf(fullpath, sizeof(fullpath), "%s/%s", cwd, name);
        if (check_exec(fullpath)) {
            printf("%s\n", fullpath);
            found = 1;
        }
    }

    /* ── 2. Walk each directory in PATH, in order ───────────────────── */
    const char *path_env = getenv("PATH");
    if (path_env == NULL) return found;

    char *path_copy = strdup(path_env);
    if (path_copy == NULL) return found;

    char *dir = strtok(path_copy, ":");
    while (dir != NULL) {
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, name);
        if (check_exec(fullpath)) {
            printf("%s\n", fullpath);
            found = 1;
        }
        dir = strtok(NULL, ":");
    }
    free(path_copy);

    return found;
}

/* ------------------------------------------------------------------ */
/* locate — main entry point                                          */
/* ------------------------------------------------------------------ */
void locate(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "locate: invalid syntax\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        if (!locate_one(argv[i]))
            fprintf(stderr, "locate: command not found (%s)\n", argv[i]);
    }
}
