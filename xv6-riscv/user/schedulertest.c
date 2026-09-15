#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Fixed, mixed workload used to compare the three schedulers and, under MLFQ, to trace queue
// movement over time. The same four children, forked in the same order, are used every time this
// program is run, so every scheduler build sees an identical set of processes.
//
//   pid (in fork order)   behaviour                                     role in the workload
//   1st child             9,000,000,000 loop iterations, no waiting     heavy CPU-bound (~150
//                                                                       ticks of work, calibrated
//                                                                       standalone at ~60M
//                                                                       iterations/tick)
//   2nd child             5,400,000,000 loop iterations, no waiting     medium CPU-bound (~90
//                                                                       ticks of work)
//   3rd child             30 short CPU bursts, pausing 3 ticks each     I/O-bound (long waits)
//   4th child             40 short CPU bursts, pausing 2 ticks each     I/O-bound (short waits)
//
// The CPU-bound children give the MLFQ scheduler enough runtime to demote them through several
// queues and to show more than one 48-tick priority boost. The I/O-bound children sleep before
// using up even their first (1-tick) slice, so under MLFQ they should stay in queue 0 throughout,
// the behaviour the assignment asks to be visible on the plot.
//
// Run this under SCHEDULER=MLFQ with STATS=1 to collect QLOG lines for the timeline plot, or
// under any scheduler with STATS=1 to collect SCHEDSTAT lines (one per child, printed by the
// kernel in kexit()) for the turnaround/waiting/response time comparison.

// Runs a fixed amount of computation with no clock checks at all, so how long this takes in wall
// time depends entirely on how much CPU service the scheduler gives it. (An earlier version of
// this function spun until a wall-clock tick target was reached, which was a mistake: that target
// is reached at close to the same real time under every scheduler, since it only depends on the
// global tick counter advancing, not on how much CPU this process actually got. That made the
// three schedulers look almost identical on turnaround time. A fixed amount of work does not have
// that problem: under a scheduler that makes this process wait longer, it simply takes longer.)
static void
cpu_bound(long iterations)
{
  for (volatile long i = 0; i < iterations; i++)
    ;
  exit(0);
}

static void
io_bound(int bursts, int pause_ticks)
{
  for (int i = 0; i < bursts; i++) {
    for (volatile int j = 0; j < 20000; j++) // a small amount of real work between waits
      ;
    pause(pause_ticks);
  }
  exit(0);
}

int
main(void)
{
  if (fork() == 0)
    cpu_bound(9000000000L);
  if (fork() == 0)
    cpu_bound(5400000000L);
  if (fork() == 0)
    io_bound(30, 3);
  if (fork() == 0)
    io_bound(40, 2);

  for (int i = 0; i < 4; i++)
    wait(0);

  printf("schedulertest done\n");
  exit(0);
}
