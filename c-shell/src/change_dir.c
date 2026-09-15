#include "shell.h"

/* Previous working directory, used for the dash argument to hop. */
static char prev_path[1024] = "";

const char *get_prev_path(void) { return prev_path; }

/* Computes a frecency score for a directory entry, combining how often
 * and how recently it has been visited. The raw visit count is
 * multiplied by a time-based factor: a four times boost if visited
 * within the last hour, a two times boost within the last day, a half
 * penalty within the last week, and a quarter penalty beyond that. This
 * way a directory visited ten times yesterday outranks one visited
 * twenty times a month ago. */
double frecency_score(HopEntry *e) {
  time_t now = time(NULL);
  double age = difftime(now, e->last_visit);
  double multiplier;

  if (age < 3600.0)
    multiplier = 4.0;
  else if (age < 86400.0)
    multiplier = 2.0;
  else if (age < 604800.0)
    multiplier = 0.5;
  else
    multiplier = 0.25;

  return e->score * multiplier;
}

/* Keeps the frecency database from growing without bound. If the sum
 * of every entry's raw score exceeds ZO_MAXAGE, every score is scaled
 * down so the new total is about ninety percent of ZO_MAXAGE, and any
 * entry whose score then falls below 1.0 is dropped. This keeps old,
 * rarely used directories from clogging up the database forever. */
void age_db(HopEntry *db, int *db_size) {
  double total = 0.0;
  for (int i = 0; i < *db_size; i++)
    total += db[i].score;

  if (total <= ZO_MAXAGE)
    return;

  double k = total / (0.9 * ZO_MAXAGE);
  for (int i = 0; i < *db_size; i++)
    db[i].score /= k;

  int write = 0;
  for (int read = 0; read < *db_size; read++) {
    if (db[read].score >= 1.0)
      db[write++] = db[read];
  }
  *db_size = write;
}

/* Removes entries for directories that no longer exist on disk, but
 * only once they have not been visited in the last ninety days. This
 * avoids deleting entries for a directory that might simply be on a
 * temporarily unmounted filesystem. */
void prune_db(HopEntry *db, int *db_size) {
  time_t now = time(NULL);
  int write = 0;

  for (int read = 0; read < *db_size; read++) {
    struct stat st;
    int path_exists = (stat(db[read].path, &st) == 0 && S_ISDIR(st.st_mode));

    if (path_exists) {
      db[write++] = db[read];
      continue;
    }

    double age = difftime(now, db[read].last_visit);
    if (age <= PRUNE_AGE_SECS)
      db[write++] = db[read];
  }

  *db_size = write;
}

/* Loads the frecency database from the history file at startup. If the
 * file does not exist yet, the database simply starts empty. After
 * loading, prune_db removes any stale entries. */
void load_hop_db(HopEntry *db, int *db_size) {
  *db_size = 0;

  char *home = getenv("HOME");
  if (home == NULL)
    return;

  char db_path[1024];
  snprintf(db_path, sizeof(db_path), "%s%s", home, HOP_DB_PATH);

  FILE *f = fopen(db_path, "rb");
  if (f == NULL)
    return;

  while (*db_size < MAX_HOP_ENTRIES) {
    if (fread(&db[*db_size], sizeof(HopEntry), 1, f) != 1)
      break;
    (*db_size)++;
  }
  fclose(f);

  prune_db(db, db_size);
}

/* Persists the frecency database to the history file on a clean exit.
 * A failure to write is reported but otherwise ignored, since the
 * database is only advisory and losing it costs nothing but the
 * ranking history. */
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

/* Updates the frecency database after a successful hop. If the
 * directory is already recorded, its score is increased and its
 * timestamp refreshed; otherwise a new entry is added if there is
 * room. Either way, age_db is run afterward to keep the total score in
 * check. */
void record_visit(HopEntry *db, int *db_size, const char *abs_path) {
  for (int i = 0; i < *db_size; i++) {
    if (strcmp(db[i].path, abs_path) == 0) {
      db[i].score += 1.0;
      db[i].last_visit = time(NULL);
      age_db(db, db_size);
      return;
    }
  }

  if (*db_size < MAX_HOP_ENTRIES) {
    strncpy(db[*db_size].path, abs_path, sizeof(db[*db_size].path) - 1);
    db[*db_size].path[sizeof(db[*db_size].path) - 1] = '\0';
    db[*db_size].score = 1.0;
    db[*db_size].last_visit = time(NULL);
    (*db_size)++;
    age_db(db, db_size);
  }
}

/* Wraps chdir with the bookkeeping hop needs: it saves the current
 * directory for the dash shortcut, resolves the real absolute path of
 * the destination since the argument might have been relative, and
 * records the visit in the frecency database. Returns 0 on success, -1
 * on failure. */
static int do_chdir(const char *target, HopEntry *db, int *db_size) {
  char cwd_before[1024];
  if (getcwd(cwd_before, sizeof(cwd_before)) == NULL) {
    perror("hop: getcwd");
    return -1;
  }

  if (chdir(target) != 0)
    return -1;

  char cwd_after[1024];
  if (getcwd(cwd_after, sizeof(cwd_after)) == NULL) {
    perror("hop: getcwd");
    return -1;
  }

  strncpy(prev_path, cwd_before, sizeof(prev_path) - 1);
  prev_path[sizeof(prev_path) - 1] = '\0';

  record_visit(db, db_size, cwd_after);
  return 0;
}

/* Scans the frecency database for entries whose path contains the
 * query string as a substring, and returns the index of whichever
 * matching entry has the highest frecency score, or -1 if none match.
 * This is the fuzzy jump feature, where typing part of a path lets hop
 * guess which directory was meant. Entries already marked in tried
 * were attempted and failed, so they are skipped. */
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

/* Applies a single hop argument, where NULL means no argument was
 * given. There are five cases: no argument or a tilde goes to the
 * shell's home directory; a single dot stays in place; a double dot
 * goes to the parent directory, or does nothing at the filesystem
 * root; a dash goes to the previous directory, or does nothing if
 * there is none; and any other name is first tried as a direct
 * relative or absolute path, and if that fails, as a frecency
 * substring match. If nothing works, an error is printed. Every
 * successful hop updates the frecency database and records the
 * previous directory for the dash shortcut. */
static void hop_one(const char *arg, HopEntry *db, int *db_size) {

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

  if (strcmp(arg, ".") == 0)
    return;

  if (strcmp(arg, "..") == 0) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      perror("hop: getcwd");
      return;
    }
    if (strcmp(cwd, "/") == 0)
      return;
    if (do_chdir("..", db, db_size) != 0)
      fprintf(stderr, "hop: no such directory\n");
    return;
  }

  if (strcmp(arg, "-") == 0) {
    if (prev_path[0] == '\0')
      return;
    if (do_chdir(prev_path, db, db_size) != 0)
      fprintf(stderr, "hop: no such directory\n");
    return;
  }

  if (do_chdir(arg, db, db_size) == 0)
    return;

  /* If the best-scoring match no longer exists on disk, do_chdir fails
   * without touching the database, so it is marked tried and the next
   * best match is tried instead. */
  char tried[MAX_HOP_ENTRIES] = {0};
  int best;
  while ((best = frecency_lookup(arg, db, *db_size, tried)) >= 0) {
    if (do_chdir(db[best].path, db, db_size) == 0)
      return;
    tried[best] = 1;
  }

  fprintf(stderr, "hop: no such directory\n");
}

/* Implements the hop builtin. With no arguments it goes to the shell's
 * home directory; otherwise each argument is applied in order, for
 * example "hop a .. -", so several hops can be chained on one line. */
void hop(int argc, char **argv, HopEntry *db, int *db_size) {
  if (argc < 2) {
    hop_one(NULL, db, db_size);
    return;
  }

  for (int i = 1; i < argc; i++)
    hop_one(argv[i], db, db_size);
}
