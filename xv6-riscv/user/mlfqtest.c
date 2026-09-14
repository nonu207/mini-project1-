#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Temporary MLFQ test: two CPU-bound children and one I/O-bound child.
static unsigned long
spin(int nticks)
{
  int start = uptime();
  unsigned long x = 0;
  while (uptime() - start < nticks) {
    for (int i = 0; i < 1000000; i++)
      x++;
  }
  return x;
}

int
main(void)
{
  int pid;

  for (int c = 0; c < 2; c++) {
    pid = fork();
    if (pid == 0) {
      spin(120);
      printf("cpu child %d finished\n", getpid());
      exit(0);
    }
    printf("started cpu child %d\n", pid);
  }

  pid = fork();
  if (pid == 0) {
    for (int i = 0; i < 40; i++)
      pause(2);
    printf("io child %d finished\n", getpid());
    exit(0);
  }
  printf("started io child %d\n", pid);

  for (int i = 0; i < 3; i++)
    wait(0);
  printf("mlfqtest done\n");
  exit(0);
}
