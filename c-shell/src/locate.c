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
/* build_candidate: join a directory and a command name into an        */
/* absolute path in `out`.                                             */
/*                                                                      */
/* `dir` is the first `dir_len` bytes of a PATH entry, following POSIX */
/* rules: an empty entry (or ".") means the current directory, and a   */
/* relative entry is taken relative to it. Trailing slashes are        */
/* dropped and "/" contributes no prefix, so "/bin/" or a cwd of "/"   */
/* never produce "//".                                                 */
/*                                                                      */
/* `cwd` may be NULL if it is unknown; entries that need it then fail. */
/* Returns 0 on success, -1 if the path can't be built or won't fit.   */
/* ------------------------------------------------------------------ */
static int build_candidate(const char *dir, size_t dir_len, const char *cwd,
                           const char *name, char *out, size_t out_size) {
    /* Drop trailing slashes, but keep a lone "/" */
    while (dir_len > 1 && dir[dir_len - 1] == '/')
        dir_len--;

    int cwd_needed = (dir_len == 0 || dir[0] != '/');
    if (cwd_needed && cwd == NULL)
        return -1;

    /* A base of exactly "/" adds nothing before the joining '/' */
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

    /* Skip, rather than probe, a path truncated to fit */
    if (len < 0 || (size_t)len >= out_size)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* locate_one: search for a single filename and print all matches.    */
/* Returns 1 if at least one match was found, 0 if none.              */
/* ------------------------------------------------------------------ */
static int locate_one(const char *name) {
    int found = 0;
    char fullpath[PATH_MAX];

    char cwd_buf[PATH_MAX];
    const char *cwd = getcwd(cwd_buf, sizeof(cwd_buf));

    /* ── 1. Check the current working directory first ──────────────── */
    if (build_candidate("", 0, cwd, name, fullpath, sizeof(fullpath)) == 0 &&
        check_exec(fullpath)) {
        printf("%s\n", fullpath);
        found = 1;
    }

    /* ── 2. Walk each directory in PATH, in order ───────────────────── */
    const char *path_env = getenv("PATH");
    if (path_env == NULL) return found;

    /* Split on ':' by hand: strtok would skip empty entries, which
     * POSIX defines as the current directory. */
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
