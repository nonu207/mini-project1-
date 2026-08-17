#ifndef PROC_H
#define PROC_H

/* Priority Queue Levels for MLFQ */
#define NPRIORITY 4
#define BOOST_INTERVAL 48

/* Time slice ticks per queue */
/* Priority 0: 1 tick  */
/* Priority 1: 4 ticks */
/* Priority 2: 8 ticks */
/* Priority 3: 16 ticks */

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

/* Per-process state structure extension for MLFQ */
struct proc {
  int pid;                     /* Process ID */
  enum procstate state;        /* Process state */
  int priority;                /* Current priority queue (0 highest, 3 lowest) */
  int ticks_consumed;          /* Ticks consumed in current time slice */
  int ticks_since_boost;       /* Ticks accumulated since last priority boost */
  int arrival_time;            /* Tick timestamp when process entered ready state */
  int start_time;              /* Tick timestamp when process first ran */
  int end_time;                /* Tick timestamp when process completed */
  char name[16];               /* Process name */
};

void scheduler(void);
void procdump(void);

#endif /* PROC_H */
