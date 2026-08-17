#include "proc.h"
#include <stdio.h>

void scheduler(void) {
#if defined(MLFQ)
    /* MLFQ Scheduler Loop implementation */
    /* 1. Pick process from highest priority non-empty queue (0 to 3) */
    /* 2. Run for designated time slice ticks */
    /* 3. Priority boost every 48 ticks */
#else
    /* Default Round Robin Scheduler Loop */
#endif
}

void procdump(void) {
    printf("PID\tName\tState\tPriority\tTicks\n");
    /* Extended procdump logic to inspect process priority queue status */
}
