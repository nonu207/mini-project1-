/* User-space test program to benchmark FIFO, Round Robin, and MLFQ schedulers */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#define NPROCS 5

void cpu_bound_work(int id) {
    long volatile count = 0;
    for (long i = 0; i < 100000000L; i++) {
        count += i;
    }
    printf("[Child %id] CPU-bound work completed\n", id);
}

void io_bound_work(int id) {
    for (int i = 0; i < 10; i++) {
        usleep(10000);
    }
    printf("[Child %id] I/O-bound work completed\n", id);
}

int main(int argc, char *argv[]) {
    printf("Starting Scheduler Test Benchmark...\n");

    for (int i = 0; i < NPROCS; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        } else if (pid == 0) {
            if (i % 2 == 0) {
                cpu_bound_work(i);
            } else {
                io_bound_work(i);
            }
            exit(0);
        }
    }

    for (int i = 0; i < NPROCS; i++) {
        wait(NULL);
    }

    printf("Scheduler Test Benchmark completed.\n");
    return 0;
}
