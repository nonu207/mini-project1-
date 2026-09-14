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
/* Computes a "frecency" score for a directory entry. The idea is to
 * rank directories by how often and how recently they've been visited.
 * Raw score gets multiplied by a time-based factor:
 *   - visited within the last hour:   4x boost
 *   - visited within the last day:    2x boost
 *   - visited within the last week:   0.5x penalty
 *   - older than a week:              0.25x penalty
 * This way a directory you used 10 times yesterday beats one you used
 * 20 times last month. */
double frecency_score(HopEntry *e) {
  time_t now = time(NULL);
  double age = difftime(now, e->last_visit); /* seconds since last visit */
  double multiplier;

  if (age < 3600.0)
    multiplier = 4.0; /* < 1 hour  */
  else if (age < 86400.0)
    multiplier = 2.0; /* < 1 day   */
  else if (age < 604800.0)
    multiplier = 0.5; /* < 1 week  */
  else
    multiplier = 0.25; /* older     */

  return e->score * multiplier;
}

/* ------------------------------------------------------------------ */
/* age_db                                                               */
/*                                                                      */
/* Keeps the frecency database from growing unbounded. If the sum of
 * all raw scores exceeds ZO_MAXAGE, we scale everything down so the
 * total becomes ~90% of MAXAGE. Then we drop any entries whose score
 * fell below 1.0. This prevents old, rarely-used directories from
 * clogging up the database forever. */
void age_db(HopEntry *db, int *db_size) {
  double total = 0.0;
  for (int i = 0; i < *db_size; i++)
    total += db[i].score;

  if (total <= ZO_MAXAGE)
    return; /* Nothing to do */

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
/* Lazily removes entries for directories that no longer exist on
 * disk, but only if they haven't been visited in the last 90 days.
 * This avoids nuking entries for directories that might be on a
 * temporarily-unmounted filesystem or similar. */
void prune_db(HopEntry *db, int *db_size) {
  time_t now = time(NULL);
  int write = 0;

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
/* Loads the frecency database from ~/.cshell_hop_db at startup.
 * If the file doesn't exist yet, that's fine — we just start fresh.
 * After loading, we run prune_db to clean up any stale entries. */
void load_hop_db(HopEntry *db, int *db_size) {
  *db_size = 0;

  char *home = getenv("HOME");
  if (home == NULL)
    return;

  char db_path[1024];
  snprintf(db_path, sizeof(db_path), "%s%s", home, HOP_DB_PATH);

  FILE *f = fopen(db_path, "rb");
  if (f == NULL)
    return; /* No history yet — fine */

  while (*db_size < MAX_HOP_ENTRIES) {
    if (fread(&db[*db_size], sizeof(HopEntry), 1, f) != 1)
      break;
    (*db_size)++;
  }
  fclose(f);

  /* Lazily prune stale entries on every load */
  prune_db(db, db_size);
}

/* ------------------------------------------------------------------ */
/* save_hop_db                                                          */
/* ------------------------------------------------------------------ */
/* Persists the frecency database to ~/.cshell_hop_db on clean exit.
 * We don't sweat it if the write fails — just print a warning and
 * keep going. The database is advisory anyway. */
void save_hop_db(HopEntry *db, int db_size) {
  char *home = getenv("HOME");
  if (home == NULL)
    return;

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
/* Updates the frecency database after a successful hop. If the
 * directory is already in the database, we bump its score and refresh
 * the timestamp. Otherwise we add a new entry (if there's room).
 * Then we run age_db to keep the total score in check. */
void record_visit(HopEntry *db, int *db_size, const char *abs_path) {
  /* Update existing entry */
  for (int i = 0; i < *db_size; i++) {
    if (strcmp(db[i].path, abs_path) == 0) {
      db[i].score += 1.0;
      db[i].last_visit = time(NULL);
      age_db(db, db_size); /* Check aging on every update */
      return;
    }
  }

  /* Insert new entry */
  if (*db_size < MAX_HOP_ENTRIES) {
    strncpy(db[*db_size].path, abs_path, sizeof(db[*db_size].path) - 1);
    db[*db_size].path[sizeof(db[*db_size].path) - 1] = '\0';
    db[*db_size].score = 1.0;
    db[*db_size].last_visit = time(NULL);
    (*db_size)++;
    age_db(db, db_size); /* Check aging on every insertion */
  }
}

/* ------------------------------------------------------------------ */
/* Internal: chdir helper — saves prev_path and records the visit.     */
/* Returns 0 on success, -1 on failure.                                */
/* ------------------------------------------------------------------ */
/* Wraps chdir() with the bookkeeping we need: save the old directory
 * for the "-" shortcut, resolve the real absolute path (since the
 * argument might be relative), and record the visit in the frecency
 * database. */
static int do_chdir(const char *target, HopEntry *db, int *db_size) {
  char cwd_before[1024];
  if (getcwd(cwd_before, sizeof(cwd_before)) == NULL) {
    perror("hop: getcwd");
    return -1;
  }

  if (chdir(target) != 0)
    return -1;

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
/* Scans the database for entries whose path contains the query string
 * as a substring. Among those, picks the one with the highest frecency
 * score. This is the "fuzzy jump" feature — type part of a path and
 * hop figures out which directory you probably meant. Entries marked
 * in tried[] were already attempted and failed, so they are skipped. */
static int frecency_lookup(const char *query, HopEntry *db, int db_size,
                           const char *tried) {
  int best_idx = -1;
  double best_score = -1.0;

  for (int i = 0; i < db_size; i++) {
    if (tried[i] || strstr(db[i].path, query) == NULL)
      continue;

    double s = frecency_score(&db[i]);
    if (s > best_score) {
      best_score = s;
      best_idx = i;
    }
  }
  return best_idx;
}

/* ------------------------------------------------------------------ */
/* hop_one: apply a single hop argument (NULL means no argument).      */
/* ------------------------------------------------------------------ */
/* Handles five cases:
 *
 * 1. No argument or "~" → jump to the shell's home directory.
 * 2. "." → stay put (no-op).
 * 3. ".." → go to parent (no-op at root).
 * 4. "-" → go to previous directory (no-op if none saved).
 * 5. "name" → try as a direct path first (relative or absolute).
 *    If that fails, fall back to frecency substring match.
 *    If neither works, print "hop: no such directory".
 *
 * Each successful hop updates the frecency database and saves the
 * previous directory for the "-" shortcut. */
static void hop_one(const char *arg, HopEntry *db, int *db_size) {

  /* ── Case 1: no argument or "~" → go to shell's home dir ─────── */
  if (arg == NULL || strcmp(arg, "~") == 0) {
    const char *home = get_shell_home();
    if (home == NULL || home[0] == '\0') {
      fprintf(stderr, "hop: shell home not set\n");
      return;
    }
    if (do_chdir(home, db, db_size) != 0)
      fprintf(stderr, "hop: no such directory\n");
    return;
  }

  /* ── Case 2: "." → stay in place ──────────────────────────────── */
  if (strcmp(arg, ".") == 0)
    return;

  /* ── Case 3: ".." → go to parent (no-op at root) ──────────────── */
  if (strcmp(arg, "..") == 0) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      perror("hop: getcwd");
      return;
    }
    if (strcmp(cwd, "/") == 0)
      return; /* already at root */
    if (do_chdir("..", db, db_size) != 0)
      fprintf(stderr, "hop: no such directory\n");
    return;
  }

  /* ── Case 4: "-" → go to previous directory ───────────────────── */
  if (strcmp(arg, "-") == 0) {
    if (prev_path[0] == '\0')
      return; /* no previous dir, do nothing */
    if (do_chdir(prev_path, db, db_size) != 0)
      fprintf(stderr, "hop: no such directory\n");
    return;
  }

  /* ── Case 5: "name" → direct path, then frecency fallback ─────── */

  /* 5a. Try as a direct relative or absolute path */
  if (do_chdir(arg, db, db_size) == 0)
    return;

  /* 5b. Frecency fallback: best-scoring substring match. If the top
   * match no longer exists on disk, skip it and try the next best.
   * A failed do_chdir leaves db untouched, so indices stay valid. */
  char tried[MAX_HOP_ENTRIES] = {0};
  int best;
  while ((best = frecency_lookup(arg, db, *db_size, tried)) >= 0) {
    if (do_chdir(db[best].path, db, db_size) == 0)
      return;
    tried[best] = 1;
  }

  /* 5c. Nothing matched */
  fprintf(stderr, "hop: no such directory\n");
}

/* ------------------------------------------------------------------ */
/* hop                                                                  */
/* ------------------------------------------------------------------ */
/* Implements the hop builtin: with no arguments, go home; otherwise
 * apply each argument in order (e.g. "hop a .. -"). A failing argument
 * prints its error and the remaining arguments still run. */
void hop(int argc, char **argv, HopEntry *db, int *db_size) {
  if (argc < 2) {
    hop_one(NULL, db, db_size);
    return;
  }

  for (int i = 1; i < argc; i++)
    hop_one(argv[i], db, db_size);
}