#include "shell.h"

/* ------------------------------------------------------------------ */
/* Internal state                                                       */
/* ------------------------------------------------------------------ */

/* Previous working directory (for the "-" case). */
static char prev_path[1024] = "";

const char *get_prev_path(void) { return prev_path; }

/* ------------------------------------------------------------------ */
/* frecency_score                                                       */
/* ------------------------------------------------------------------ */

double frecency_score(HopEntry *e) {
    time_t now = time(NULL);
    double age = difftime(now, e->last_visit); /* seconds since last visit */
    double multiplier;

    if      (age < 3600.0)   multiplier = 4.0;   /* < 1 hour  */
    else if (age < 86400.0)  multiplier = 2.0;   /* < 1 day   */
    else if (age < 604800.0) multiplier = 0.5;   /* < 1 week  */
    else                     multiplier = 0.25;  /* older     */

    return e->score * multiplier;
}

/* ------------------------------------------------------------------ */
/* age_db                                                               */
/*                                                                      */
/* If the total raw score exceeds ZO_MAXAGE, divide every score by k  */
/* so the new total becomes ~90% of ZO_MAXAGE. Then remove any entry   */
/* whose score falls below 1.0.                                        */
/* ------------------------------------------------------------------ */

void age_db(HopEntry *db, int *db_size) {
    double total = 0.0;
    for (int i = 0; i < *db_size; i++)
        total += db[i].score;

    if (total <= ZO_MAXAGE) return; /* Nothing to do */

    /* k scales the total down to 90 % of MAXAGE */
    double k = total / (0.9 * ZO_MAXAGE);
    for (int i = 0; i < *db_size; i++)
        db[i].score /= k;

    /* Compact: drop entries that fell below 1.0 */
    int write = 0;
    for (int read = 0; read < *db_size; read++) {
        if (db[read].score >= 1.0)
            db[write++] = db[read];
    }
    *db_size = write;
}

/* ------------------------------------------------------------------ */
/* prune_db                                                             */
/*                                                                      */
/* Lazily remove entries whose path no longer exists on the filesystem  */
/* AND whose last_visit is older than PRUNE_AGE_SECS (90 days).        */
/* ------------------------------------------------------------------ */

void prune_db(HopEntry *db, int *db_size) {
    time_t now  = time(NULL);
    int    write = 0;

    for (int read = 0; read < *db_size; read++) {
        struct stat st;
        int path_exists = (stat(db[read].path, &st) == 0 && S_ISDIR(st.st_mode));

        if (path_exists) {
            db[write++] = db[read]; /* Path still valid — always keep */
            continue;
        }

        /* Path is gone — only prune if the entry is also old */
        double age = difftime(now, db[read].last_visit);
        if (age <= PRUNE_AGE_SECS)
            db[write++] = db[read]; /* Recent enough — keep it for now */
        /* else: gone AND older than 90 days → silently drop */
    }

    *db_size = write;
}

/* ------------------------------------------------------------------ */
/* load_hop_db                                                          */
/* ------------------------------------------------------------------ */

void load_hop_db(HopEntry *db, int *db_size) {
    *db_size = 0;

    char *home = getenv("HOME");
    if (home == NULL) return;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s%s", home, HOP_DB_PATH);

    FILE *f = fopen(db_path, "rb");
    if (f == NULL) return; /* No history yet — fine */

    while (*db_size < MAX_HOP_ENTRIES) {
        if (fread(&db[*db_size], sizeof(HopEntry), 1, f) != 1) break;
        (*db_size)++;
    }
    fclose(f);

    /* Lazily prune stale entries on every load */
    prune_db(db, db_size);
}

/* ------------------------------------------------------------------ */
/* save_hop_db                                                          */
/* ------------------------------------------------------------------ */

void save_hop_db(HopEntry *db, int db_size) {
    char *home = getenv("HOME");
    if (home == NULL) return;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s%s", home, HOP_DB_PATH);

    FILE *f = fopen(db_path, "wb");
    if (f == NULL) {
        perror("hop: could not save history");
        return;
    }
    fwrite(db, sizeof(HopEntry), db_size, f);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* record_visit                                                         */
/* ------------------------------------------------------------------ */

void record_visit(HopEntry *db, int *db_size, const char *abs_path) {
    /* Update existing entry */
    for (int i = 0; i < *db_size; i++) {
        if (strcmp(db[i].path, abs_path) == 0) {
            db[i].score     += 1.0;
            db[i].last_visit = time(NULL);
            age_db(db, db_size); /* Check aging on every update */
            return;
        }
    }

    /* Insert new entry */
    if (*db_size < MAX_HOP_ENTRIES) {
        strncpy(db[*db_size].path, abs_path, sizeof(db[*db_size].path) - 1);
        db[*db_size].path[sizeof(db[*db_size].path) - 1] = '\0';
        db[*db_size].score      = 1.0;
        db[*db_size].last_visit = time(NULL);
        (*db_size)++;
        age_db(db, db_size); /* Check aging on every insertion */
    }
}

/* ------------------------------------------------------------------ */
/* Internal: chdir helper — saves prev_path and records the visit.     */
/* Returns 0 on success, -1 on failure.                                */
/* ------------------------------------------------------------------ */

static int do_chdir(const char *target, HopEntry *db, int *db_size) {
    char cwd_before[1024];
    if (getcwd(cwd_before, sizeof(cwd_before)) == NULL) {
        perror("hop: getcwd");
        return -1;
    }

    if (chdir(target) != 0) return -1;

    /* Resolve the real new path (target may have been relative) */
    char cwd_after[1024];
    if (getcwd(cwd_after, sizeof(cwd_after)) == NULL) {
        perror("hop: getcwd");
        return -1;
    }

    /* Save for "-" */
    strncpy(prev_path, cwd_before, sizeof(prev_path) - 1);
    prev_path[sizeof(prev_path) - 1] = '\0';

    record_visit(db, db_size, cwd_after);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Internal: frecency best-match lookup.                               */
/* Returns index of best matching entry, or -1 if none.               */
/* ------------------------------------------------------------------ */

static int frecency_lookup(const char *query, HopEntry *db, int db_size) {
    int    best_idx   = -1;
    double best_score = -1.0;

    for (int i = 0; i < db_size; i++) {
        if (strstr(db[i].path, query) == NULL) continue;

        double s = frecency_score(&db[i]);
        if (s > best_score) {
            best_score = s;
            best_idx   = i;
        }
    }
    return best_idx;
}

/* ------------------------------------------------------------------ */
/* hop                                                                  */
/* ------------------------------------------------------------------ */

void hop(int argc, char **argv, HopEntry *db, int *db_size) {

    /* ── Case 1: no argument or "~" → go to shell's home dir ─────── */
    if (argc < 2 || strcmp(argv[1], "~") == 0) {
        const char *home = get_shell_home();
        if (home == NULL || home[0] == '\0') {
            fprintf(stderr, "hop: shell home not set\n");
            return;
        }
        if (do_chdir(home, db, db_size) != 0)
            fprintf(stderr, "hop: no such directory\n");
        return;
    }

    char *arg = argv[1];

    /* ── Case 2: "." → stay in place ──────────────────────────────── */
    if (strcmp(arg, ".") == 0) return;

    /* ── Case 3: ".." → go to parent (no-op at root) ──────────────── */
    if (strcmp(arg, "..") == 0) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) == NULL) { perror("hop: getcwd"); return; }
        if (strcmp(cwd, "/") == 0) return; /* already at root */
        if (do_chdir("..", db, db_size) != 0)
            fprintf(stderr, "hop: no such directory\n");
        return;
    }

    /* ── Case 4: "-" → go to previous directory ───────────────────── */
    if (strcmp(arg, "-") == 0) {
        if (prev_path[0] == '\0') return; /* no previous dir, do nothing */
        if (do_chdir(prev_path, db, db_size) != 0)
            fprintf(stderr, "hop: no such directory\n");
        return;
    }

    /* ── Case 5: "name" → direct path, then frecency fallback ─────── */

    /* 5a. Try as a direct relative or absolute path */
    if (do_chdir(arg, db, db_size) == 0) return;

    /* 5b. Frecency fallback: best-scoring substring match */
    int best = frecency_lookup(arg, db, *db_size);
    if (best >= 0 && do_chdir(db[best].path, db, db_size) == 0) return;

    /* 5c. Nothing matched */
    fprintf(stderr, "hop: no such directory\n");
}