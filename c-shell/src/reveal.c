#include "shell.h"

/* Resolves a path argument using hop's rules, but without the frecency
 * fallback, since reveal never falls back to a fuzzy match. Fills
 * out_path with the absolute path on success. Returns -1 if the path
 * cannot be resolved or does not name a directory. */
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
        /* A dot, a double dot, or a relative or absolute path are all
         * handled by realpath. */
        target = arg;
    }

    if (realpath(target, out_path) == NULL) return -1;

    struct stat st;
    if (stat(out_path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;

    return 0;
}

/* Comparator for sorting an array of string pointers with qsort. */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Reads the entries of a directory into a heap-allocated array of
 * strdup'd names, sorted lexicographically, and sets count to the
 * number of entries. The dot and dot-dot entries are always skipped,
 * and hidden entries are skipped unless show_hidden is set. The caller
 * must free each name and the array itself. */
static char **read_sorted_entries(const char *abs_path, int show_hidden, int *count) {
    DIR *dp = opendir(abs_path);
    if (dp == NULL) { *count = 0; return NULL; }

    char  **entries  = NULL;
    int     cap      = 0;
    *count           = 0;

    struct dirent *ep;
    while ((ep = readdir(dp)) != NULL) {
        const char *name = ep->d_name;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!show_hidden && name[0] == '.') continue;

        if (*count >= cap) {
            cap      = cap == 0 ? 16 : cap * 2;
            entries  = realloc(entries, cap * sizeof(char *));
        }
        entries[(*count)++] = strdup(name);
    }
    closedir(dp);

    if (*count > 0)
        qsort(entries, *count, sizeof(char *), cmp_str);

    return entries;
}

/* Prints the bare, sorted names of a directory's entries, one per line,
 * with no recursion into subdirectories. */
static void list_flat(const char *abs_path, int show_hidden) {
    int    count;
    char **entries = read_sorted_entries(abs_path, show_hidden, &count);

    for (int i = 0; i < count; i++) {
        printf("%s\n", entries[i]);
        free(entries[i]);
    }
    free(entries);
}

/* Recursively lists a directory's contents. Each entry is printed as a
 * path relative to the starting directory, and a directory entry is
 * printed immediately followed by its own contents, so the listing is
 * depth-first and lexicographic at every level.
 *
 * abs_path is the absolute path of the directory currently being
 * listed, prefix is the relative path built up so far, which is empty
 * at the root, and show_hidden controls whether hidden entries are
 * included.
 *
 * Whether an entry is itself a directory is checked with lstat rather
 * than stat, so a symlink to a directory is listed as a plain entry and
 * never followed. Otherwise a link that points back up the tree, such
 * as one named loop pointing at its own parent, would cause infinite
 * recursion, which is also why plain ls -R avoids following symlinks
 * here. */
static void list_recursive(const char *abs_path, const char *prefix, int show_hidden) {
    int    count;
    char **entries = read_sorted_entries(abs_path, show_hidden, &count);

    for (int i = 0; i < count; i++) {
        char display[PATH_MAX];
        if (prefix[0] == '\0')
            snprintf(display, sizeof(display), "%s", entries[i]);
        else
            snprintf(display, sizeof(display), "%s/%s", prefix, entries[i]);

        char child_abs[PATH_MAX];
        snprintf(child_abs, sizeof(child_abs), "%s/%s", abs_path, entries[i]);

        struct stat st;
        int is_dir = (lstat(child_abs, &st) == 0 && S_ISDIR(st.st_mode));

        if (is_dir)
            printf("%s/\n", display);
        else
            printf("%s\n", display);

        /* The prefix passed down has no trailing slash, so building the
         * next level's display path as prefix/name never produces a
         * doubled slash such as include//parser.h. */
        if (is_dir)
            list_recursive(child_abs, display, show_hidden);

        free(entries[i]);
    }
    free(entries);
}

/* Implements the reveal command. Parses the a and t flags and at most
 * one path argument, resolves the target directory using the same
 * rules as hop but without a frecency fallback, and lists its contents
 * either flat or recursively depending on the t flag. */
void reveal(int argc, char **argv) {
    int   flag_a   = 0;
    int   flag_t   = 0;
    char *path_arg = NULL;
    int   path_count = 0;

    for (int i = 1; i < argc; i++) {
        char *token = argv[i];

        if (token[0] == '-' && token[1] != '\0') {
            /* Any token starting with a dash and having more characters
             * is treated as a flag; the only valid flag letters are a
             * and t. */
            for (int j = 1; token[j] != '\0'; j++) {
                if (token[j] != 'a' && token[j] != 't') {
                    fprintf(stderr, "reveal: invalid syntax\n");
                    return;
                }
            }
            for (int j = 1; token[j] != '\0'; j++) {
                if (token[j] == 'a') flag_a = 1;
                if (token[j] == 't') flag_t = 1;
            }
        } else {
            /* Anything else, including a bare dash, a tilde, a dot, a
             * double dot, or a regular path, is the path argument. */
            path_count++;
            if (path_count > 1) {
                fprintf(stderr, "reveal: invalid syntax\n");
                return;
            }
            path_arg = token;
        }
    }

    char target[PATH_MAX];

    if (path_arg == NULL) {
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

    if (flag_t)
        list_recursive(target, "", flag_a);
    else
        list_flat(target, flag_a);
}
