#include "shell.h"

/* Reports whether path is an executable regular file. */
static int check_exec(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)      return 0;
    if (S_ISDIR(st.st_mode))       return 0;
    if (access(path, X_OK) != 0)   return 0;
    return 1;
}

/* Joins a directory and a command name into an absolute path, written
 * into out. dir is the first dir_len bytes of one PATH entry, and this
 * follows POSIX rules for interpreting it: an empty entry or a single
 * dot means the current directory, and any other relative entry is
 * taken relative to the current directory. Trailing slashes are dropped
 * and a bare slash contributes no extra prefix, so joining against
 * "/bin/" or a current directory of "/" never produces a double slash.
 * cwd may be NULL if the current directory is unknown, in which case
 * any entry that needs it fails. Returns 0 on success, or -1 if the
 * path cannot be built or does not fit in out. */
static int build_candidate(const char *dir, size_t dir_len, const char *cwd,
                           const char *name, char *out, size_t out_size) {
    while (dir_len > 1 && dir[dir_len - 1] == '/')
        dir_len--;

    int cwd_needed = (dir_len == 0 || dir[0] != '/');
    if (cwd_needed && cwd == NULL)
        return -1;

    int cwd_len = cwd_needed ? (int)strlen(cwd) : 0;
    if (cwd_len == 1 && cwd[0] == '/')
        cwd_len = 0;

    int len;
    if (dir_len == 0 || (dir_len == 1 && dir[0] == '.'))
        len = snprintf(out, out_size, "%.*s/%s", cwd_len, cwd, name);
    else if (dir[0] == '/')
        len = snprintf(out, out_size, "%.*s/%s",
                       dir_len == 1 ? 0 : (int)dir_len, dir, name);
    else
        len = snprintf(out, out_size, "%.*s/%.*s/%s",
                       cwd_len, cwd, (int)dir_len, dir, name);

    if (len < 0 || (size_t)len >= out_size)
        return -1;
    return 0;
}

/* Searches for a single command name and prints the absolute path of
 * every match, first in the current working directory, then in each
 * directory listed in PATH, in order. Returns 1 if at least one match
 * was found, 0 otherwise. */
static int locate_one(const char *name) {
    int found = 0;
    char fullpath[PATH_MAX];

    char cwd_buf[PATH_MAX];
    const char *cwd = getcwd(cwd_buf, sizeof(cwd_buf));

    if (build_candidate("", 0, cwd, name, fullpath, sizeof(fullpath)) == 0 &&
        check_exec(fullpath)) {
        printf("%s\n", fullpath);
        found = 1;
    }

    const char *path_env = getenv("PATH");
    if (path_env == NULL) return found;

    /* PATH is split by hand instead of with strtok, because strtok
     * silently skips empty entries, which POSIX defines as meaning the
     * current directory. */
    const char *entry = path_env;
    for (;;) {
        const char *colon   = strchr(entry, ':');
        size_t      dir_len = colon ? (size_t)(colon - entry) : strlen(entry);

        if (build_candidate(entry, dir_len, cwd, name,
                            fullpath, sizeof(fullpath)) == 0 &&
            check_exec(fullpath)) {
            printf("%s\n", fullpath);
            found = 1;
        }

        if (colon == NULL) break;
        entry = colon + 1;
    }

    return found;
}

/* Implements the locate command. Prints the resolved path of each
 * argument in turn, or an error naming that argument if it does not
 * resolve to an executable anywhere searched. */
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
