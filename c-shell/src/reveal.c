#include "shell.h"

/* ------------------------------------------------------------------ */
/* Internal: resolve a path argument using hop's rules (no frecency). */
/*                                                                      */
/* Returns 0 and fills out_path (absolute) on success.                 */
/* Returns -1 if the path cannot be resolved or is not a directory.   */
/* ------------------------------------------------------------------ */

static int resolve_path(const char *arg, char *out_path) {
    const char *target;
    char home_buf[PATH_MAX];

    if (strcmp(arg, "~") == 0) {
        const char *home = get_shell_home();
        if (home == NULL || home[0] == '\0') return -1;
        strncpy(home_buf, home, PATH_MAX - 1);
        home_buf[PATH_MAX - 1] = '\0';
        target = home_buf;
    } else if (strcmp(arg, "-") == 0) {
        const char *prev = get_prev_path();
        if (prev == NULL || prev[0] == '\0') return -1;
        target = prev;
    } else {
        /* ".", "..", relative, or absolute — let realpath handle them all */
        target = arg;
    }

    /* realpath resolves the canonical absolute path and checks existence */
    if (realpath(target, out_path) == NULL) return -1;

    /* Must be a directory */
    struct stat st;
    if (stat(out_path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Internal: qsort comparator for string pointer arrays.              */
/* ------------------------------------------------------------------ */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ------------------------------------------------------------------ */
/* Internal: collect and sort directory entries.                        */
/* Returns a heap-allocated array of strdup'd names; sets *count.      */
/* Caller must free each name and the array itself.                    */
/* ------------------------------------------------------------------ */

static char **read_sorted_entries(const char *abs_path, int show_hidden, int *count) {
    DIR *dp = opendir(abs_path);
    if (dp == NULL) { *count = 0; return NULL; }

    char  **entries  = NULL;
    int     cap      = 0;
    *count           = 0;

    struct dirent *ep;
    while ((ep = readdir(dp)) != NULL) {
        const char *name = ep->d_name;

        /* Always skip "." and ".." */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        /* Skip hidden entries unless -a was given */
        if (!show_hidden && name[0] == '.') continue;

        if (*count >= cap) {
            cap      = cap == 0 ? 16 : cap * 2;
            entries  = realloc(entries, cap * sizeof(char *));
        }
        entries[(*count)++] = strdup(name);
    }
    closedir(dp);

    /* Sort lexicographically by ASCII value */
    if (*count > 0)
        qsort(entries, *count, sizeof(char *), cmp_str);

    return entries;
}

/* ------------------------------------------------------------------ */
/* Internal: flat listing (no -t flag).                                */
/* Prints bare entry names, one per line, sorted.                     */
/* ------------------------------------------------------------------ */

static void list_flat(const char *abs_path, int show_hidden) {
    int    count;
    char **entries = read_sorted_entries(abs_path, show_hidden, &count);

    for (int i = 0; i < count; i++) {
        printf("%s\n", entries[i]);
        free(entries[i]);
    }
    free(entries);
}

/* ------------------------------------------------------------------ */
/* Internal: recursive listing (-t flag).                              */
/*                                                                      */
/* Prints each entry as a path relative to the root target directory.  */
/* After printing a directory entry, immediately prints its contents   */
/* (depth-first, lexicographic order at each level).                   */
/*                                                                      */
/* abs_path  — absolute path of the directory being listed             */
/* prefix    — relative path accumulated so far (empty string at root) */
/* show_hidden — whether to include hidden entries                     */
/* ------------------------------------------------------------------ */

static void list_recursive(const char *abs_path, const char *prefix, int show_hidden) {
    int    count;
    char **entries = read_sorted_entries(abs_path, show_hidden, &count);

    for (int i = 0; i < count; i++) {
        /* Build the display path relative to the starting directory */
        char display[PATH_MAX];
        if (prefix[0] == '\0')
            snprintf(display, sizeof(display), "%s", entries[i]);
        else
            snprintf(display, sizeof(display), "%s/%s", prefix, entries[i]);

        /* If this entry is itself a directory, recurse into it */
        char child_abs[PATH_MAX];
        snprintf(child_abs, sizeof(child_abs), "%s/%s", abs_path, entries[i]);

        struct stat st;
        int is_dir = (stat(child_abs, &st) == 0 && S_ISDIR(st.st_mode));

        /* Print with trailing '/' for directories, plain name for files */
        if (is_dir)
            printf("%s/\n", display);
        else
            printf("%s\n", display);

        /* Recurse into subdirectories (prefix stays without '/' so that
         * child paths are built as "include/parser.h", not "include//parser.h") */
        if (is_dir)
            list_recursive(child_abs, display, show_hidden);

        free(entries[i]);
    }
    free(entries);
}

/* ------------------------------------------------------------------ */
/* reveal                                                               */
/* ------------------------------------------------------------------ */

void reveal(int argc, char **argv) {
    int   flag_a   = 0;
    int   flag_t   = 0;
    char *path_arg = NULL;
    int   path_count = 0;

    /* ── Parse flags and at most one path argument ─────────────────── */
    for (int i = 1; i < argc; i++) {
        char *token = argv[i];

        if (token[0] == '-' && token[1] != '\0') {
            /*
             * Starts with '-' and has more chars → treat as a flag.
             * Valid flag chars are only 'a' and 't'.
             */
            for (int j = 1; token[j] != '\0'; j++) {
                if (token[j] != 'a' && token[j] != 't') {
                    fprintf(stderr, "reveal: invalid syntax\n");
                    return;
                }
            }
            /* Accumulate flags (duplicates are harmless) */
            for (int j = 1; token[j] != '\0'; j++) {
                if (token[j] == 'a') flag_a = 1;
                if (token[j] == 't') flag_t = 1;
            }
        } else {
            /*
             * Not a flag: either standalone "-" (previous dir),
             * "~", ".", "..", or a regular path name.
             */
            path_count++;
            if (path_count > 1) {
                fprintf(stderr, "reveal: invalid syntax\n");
                return;
            }
            path_arg = token;
        }
    }

    /* ── Resolve target directory ──────────────────────────────────── */
    char target[PATH_MAX];

    if (path_arg == NULL) {
        /* No argument: use the current working directory */
        if (getcwd(target, sizeof(target)) == NULL) {
            fprintf(stderr, "reveal: no such directory\n");
            return;
        }
    } else {
        if (resolve_path(path_arg, target) != 0) {
            fprintf(stderr, "reveal: no such directory\n");
            return;
        }
    }

    /* ── List contents ─────────────────────────────────────────────── */
    if (flag_t)
        list_recursive(target, "", flag_a);
    else
        list_flat(target, flag_a);
}
