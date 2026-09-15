#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// FCFS test: forks a few children, each of which prints its own pid several times and exits.
// Under FCFS the child with the smallest pid should print all of its lines before the next
// child prints any of its own, since it keeps winning the scheduler's smallest-pid selection
// until it exits. Under RR or MLFQ the children's lines interleave instead.
#define NCHILD 4
#define NPRINTS 5

int
main(void)
{
  int pid;

  for (int c = 0; c < NCHILD; c++) {
    pid = fork();
    if (pid == 0) {
      int mypid = getpid();
      for (int i = 0; i < NPRINTS; i++)
        printf("child pid %d, print %d\n", mypid, i);
      exit(0);
    }
  }

  for (int c = 0; c < NCHILD; c++)
    wait(0);

  printf("testfcfs done\n");
  exit(0);
}
