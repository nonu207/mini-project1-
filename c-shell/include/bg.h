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
#define BG_CMD_MAX    512

/* One live process inside a background job. */
typedef struct {
    pid_t pid;
    char  command_name[BG_NAME_MAX];
    int   stopped;                  /* last reported state: 1 = stopped */
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
    char   cmdline[BG_CMD_MAX];     /* the job as the user typed it */
    int    lead_status;             /* wait status of the lead */
    int    lead_reaped;             /* is lead_status valid yet? */
    int    last_status;             /* fallback if the lead was never reaped */
    int    stopped;                 /* 1 while any process is stopped */
    int    nprocs;                  /* LIVE process count; 0 retires the job */
    BgProc procs[MAX_JOB_PROCS];
} BgJob;

void init_bg(void);

/* Register a whole background job.  pids[0]/names[0] must be the first
   command in the pipeline.  Prints "[job_number] pids[0]". */
void register_bg_group(pid_t pgid, const pid_t *pids,
                       char (*names)[BG_NAME_MAX], int n,
                       const char *cmdline);

/* A standalone command is a process group of one. */
void register_bg_job(pid_t pid, const char *name, const char *cmdline);

/* Reap finished children, drop them from their job, and announce any job
   whose processes have all exited.  Returns how many messages printed. */
int  check_bg_jobs(void);

/* Record that `pid` exited with wait status `status`, for a child that was
   reaped somewhere other than check_bg_jobs (snoop -p collects the exit of
   the job it traces).  Announces and retires the job if it was the last
   process.  Unknown pids are ignored.  Returns 1 if a message printed. */
int  bg_child_exited(pid_t pid, int status);

/* The SIGCHLD handler reaps (waitpid, WNOHANG) only "watched" pids: the
   processes of tracked jobs.  Jobs are watched when registered.  Code that
   waits on a job's process itself (resume fg, snoop -p) must unwatch it
   first, or the handler could reap it out from under that wait.
   bg_unwatch_pid returns 1 if the pid was being watched. */
void bg_watch_pid(pid_t pid);
int  bg_unwatch_pid(pid_t pid);

int  run_bg_group(Token *start, HopEntry *db, int *db_size);

/* In a background child, after setpgid: restore SIGTTIN/SIGTTOU so a
   terminal read stops the job, and use /dev/null as stdin when the shell
   has no terminal (so the job cannot read the shell's own input). */
void bg_child_setup(void);

/* Register a foreground job that Ctrl-Z just suspended.  Assigns the next
   job number and prints "[job_number] + Stopped command". */
void register_stopped_job(pid_t pgid, const pid_t *pids,
                          char (*names)[BG_NAME_MAX], int n,
                          const char *cmdline);

/* Is any tracked job currently stopped?  Ctrl-D consults this. */
int  bg_has_stopped(void);

/* Send SIGHUP to every tracked job's process group, without waiting.
   Called on shell exit so no job outlives the session. */
void bg_hangup_all(void);

/* ── Enumeration, for the activities built-in ──────────────────────────
   The job table is a dense array: jobs are appended on launch and the
   array is compacted on retirement, so index order IS launch order and
   bg_job_at(0) is always the oldest live job.

   The returned pointer is valid only until the next register_bg_*() or
   check_bg_jobs() call.  That is safe because the table is mutated only
   from main context -- the SIGCHLD handler reaps into its own queue and
   never touches the table.  A future handler that writes to the table
   would break this contract. */
int          bg_live_count(void);
const BgJob *bg_job_at(int idx);

/* ── Lookup and mutation, for the resume built-in ─────────────────── */

/* Find a job by the number activities prints, or NULL. */
const BgJob *bg_find_job(int job_number);

/* Clear a job's stopped flag (it has been sent SIGCONT). */
void bg_set_running(int job_number);

/* Keep only `pids` in the job and mark it stopped again. */
void bg_set_stopped(int job_number, const pid_t *pids, int n);

/* Drop a job from the table without announcing anything.  Used when a
   resumed foreground job finishes, and when a timed-out job is killed. */
void bg_remove_job(int job_number);

#endif /* BG_H */
