#ifndef CHANGE_DIR_H
#define CHANGE_DIR_H

#include <time.h>

/* Maximum number of directories stored in the frecency history */
#define MAX_HOP_ENTRIES 1024

/*
 * Maximum total score across all entries (mirrors zoxide's _ZO_MAXAGE).
 * When the total exceeds this, aging is triggered.
 */
#define ZO_MAXAGE 10000.0

/* Entries older than this many seconds with no valid path are pruned. */
#define PRUNE_AGE_SECS (90 * 24 * 3600)

/* Path to the persistent frecency history file */
#define HOP_DB_PATH "/.hop_history"

/*
 * Represents a single directory entry in the frecency history.
 *   path       - absolute path of the directory
 *   score      - cumulative visit count (incremented by 1.0 per visit)
 *   last_visit - unix timestamp of the most recent visit
 */
typedef struct {
    char   path[1024];
    double score;
    time_t last_visit;
} HopEntry;

/*
 * Computes the frecency score for an entry by applying a time-decay
 * multiplier to its raw visit score:
 *   < 1 hour  → 4.0x
 *   < 1 day   → 2.0x
 *   < 1 week  → 0.5x
 *   otherwise → 0.25x
 */
double frecency_score(HopEntry *e);

/* Load the frecency database from HOP_DB_PATH into db[]. */
void load_hop_db(HopEntry *db, int *db_size);

/* Persist the in-memory frecency database back to HOP_DB_PATH. */
void save_hop_db(HopEntry *db, int db_size);

/*
 * Aging: if the total raw score of all entries exceeds ZO_MAXAGE,
 * divide every score by k so the new total is ~90% of ZO_MAXAGE.
 * Any entry whose score then falls below 1.0 is removed.
 */
void age_db(HopEntry *db, int *db_size);

/*
 * Pruning: lazily removes entries whose path no longer exists on the
 * filesystem AND whose last_visit is older than PRUNE_AGE_SECS (90 days).
 */
void prune_db(HopEntry *db, int *db_size);

/*
 * Record a successful directory visit into the frecency database.
 * If the path already exists, increments its score and updates
 * last_visit. Otherwise creates a new entry.
 */
void record_visit(HopEntry *db, int *db_size, const char *abs_path);

/*
 * Returns the previous working directory (used by the "-" case in reveal).
 * Returns an empty string if no previous directory exists yet.
 */
const char *get_prev_path(void);

/*
 * The main hop command dispatcher.
 *
 * Handles:
 *   (no arg) / "~"  → HOME
 *   "."             → do nothing
 *   ".."            → parent directory (or do nothing at root)
 *   "-"             → previous CWD (or do nothing)
 *   "name"          → direct path, then frecency fallback
 *
 * On failure: prints "hop: no such directory"
 */
void hop(int argc, char **argv, HopEntry *db, int *db_size);

#endif /* CHANGE_DIR_H */
