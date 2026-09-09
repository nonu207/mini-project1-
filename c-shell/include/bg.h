#ifndef BG_H
#define BG_H

#include <sys/types.h>

#include "lexer.h"
#include "change_dir.h"

/* Maximum number of background jobs (process groups) tracked at once, the
   maximum number of processes in any one of them (pipeline stages), and
   the size of a stored command name. */
#define MAX_BG_JOBS   256
#define MAX_JOB_PROCS  16
#define BG_NAME_MAX   256

/* One live process inside a background job. */
typedef struct {
    pid_t pid;
    char  command_name[BG_NAME_MAX];
} BgProc;

/* One background job == one process group.  A standalone command is a
   group of one; a pipeline is a group of N sharing one pgid.

   procs[0 .. nprocs) holds only the processes still alive, in pipeline
   order; nprocs drops as stages are reaped.  The "lead" fields keep the
   identity of the first command even after it has exited, because both
   the "[N] pid" line and the completion message must name it. */
typedef struct {
    int    job_number;              /* monotonic; never reused */
    pid_t  pgid;                    /* == the first stage's pid */
    pid_t  lead_pid;
    char   lead_name[BG_NAME_MAX];
    int    lead_status;             /* wait status of the lead */
    int    lead_reaped;             /* is lead_status valid yet? */
    int    last_status;             /* fallback if the lead was never reaped */
    int    nprocs;                  /* LIVE process count; 0 retires the job */
    BgProc procs[MAX_JOB_PROCS];
} BgJob;

void init_bg(void);

/* Register a whole background job.  pids[0]/names[0] must be the first
   command in the pipeline.  Prints "[job_number] pids[0]". */
void register_bg_group(pid_t pgid, const pid_t *pids,
                       char (*names)[BG_NAME_MAX], int n);

/* A standalone command is a process group of one. */
void register_bg_job(pid_t pid, const char *name);

/* Reap finished children, drop them from their job, and announce any job
   whose processes have all exited.  Returns how many messages printed. */
int  check_bg_jobs(void);

int  run_bg_group(Token *start, HopEntry *db, int *db_size);

/* ── Enumeration, for the activities built-in ──────────────────────────
   The job table is a dense array: jobs are appended on launch and the
   array is compacted on retirement, so index order IS launch order and
   bg_job_at(0) is always the oldest live job.

   The returned pointer is valid only until the next register_bg_*() or
   check_bg_jobs() call.  That is safe because the table is mutated only
   from main context -- the SIGCHLD handler is a deliberate no-op that
   touches no shared state.  A future handler that writes to the table
   would break this contract. */
int          bg_live_count(void);
const BgJob *bg_job_at(int idx);

#endif /* BG_H */
