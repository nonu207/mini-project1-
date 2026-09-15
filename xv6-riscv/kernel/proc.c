#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

// MLFQ queue bookkeeping. mlfq_seq is the counter that hands out arrival stamps, and mlfq_lock
// protects it, because processes on different CPUs can join a queue at the same moment and must
// never get the same stamp. mlfq_enqueue() puts a process at the end of whatever queue its queue
// field says, simply by giving it the newest (largest) stamp. The caller must already hold
// p->lock; mlfq_lock is always taken last and released straight away, so it cannot cause a deadlock.
#ifdef USE_MLFQ
static uint64 mlfq_seq = 0;
static struct spinlock mlfq_lock;

// Length of the time slice, in timer ticks, for each queue. Index 0 is queue 0 (highest priority)
// and index 3 is queue 3 (lowest). Lower queues get longer slices, so processes that need a lot of
// CPU time end up running for longer stretches, less often.
static int time_slice[] = {1, 4, 8, 16};

static void
mlfq_enqueue(struct proc *p)
{
  acquire(&mlfq_lock);
  p->enq_time = mlfq_seq++;
  release(&mlfq_lock);
}
#endif

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  // Like every other kernel lock, the MLFQ stamp lock is set up once at boot, before any process
  // exists, so it is ready the first time a process joins a queue.
#ifdef USE_MLFQ
  initlock(&mlfq_lock, "mlfq");
#endif
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  // Every process slot passes through allocproc() when a new process is created, including slots
  // that were used before, so this is where we place the new process in queue 0, the highest
  // priority. It is not given its arrival stamp yet, because it is not RUNNABLE yet; that happens in
  // userinit() or kfork() once the process is fully set up.
  // Scheduler comparison bookkeeping, present under every scheduler. The process arrives now, so
  // ctime is the current tick; it has not run yet, so first_run_time and etime are both "not yet"
  // (-1), and no RUNNING or RUNNABLE ticks have accumulated.
  p->ctime = ticks;
  p->first_run_time = -1;
  p->etime = -1;
  p->rtime = 0;
  p->wtime = 0;

#ifdef USE_MLFQ
  p->queue = 0;
  // A brand new process has not run yet, so none of its first time slice has been used.
  p->ticks_used = 0;
#endif

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  // Process completion. An exiting process already left the queues in kexit(), when its state
  // became ZOMBIE, because the scheduler only ever picks RUNNABLE processes. freeproc() is called
  // when the parent collects it, and the slot becomes UNUSED. Here we also wipe its MLFQ
  // bookkeeping, so no queue level, tick count or arrival stamp from the old process is left behind
  // in a slot that will later be reused for a new process.
#ifdef USE_MLFQ
  p->queue = 0;
  p->ticks_used = 0;
  p->enq_time = 0;
#endif
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");

  p->state = RUNNABLE;
  // The very first process (init) becomes RUNNABLE here. Giving it an arrival stamp pushes it onto
  // the end of queue 0 (allocproc already set its queue to 0).
#ifdef USE_MLFQ
  mlfq_enqueue(p);
#endif

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    if (sz + n > TRAPFRAME) {
      return -1;
    }
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  // Every other new process is created by fork and becomes RUNNABLE here. It gets the newest
  // arrival stamp, so it joins the end of queue 0, behind any processes already waiting there.
#ifdef USE_MLFQ
  mlfq_enqueue(np);
#endif
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  // Under MLFQ this is the moment the process leaves the queuing system: a ZOMBIE is never RUNNABLE
  // again, so the scheduler will never choose it, and it is not put back into any queue. Its
  // leftover MLFQ fields are cleared later in freeproc().
  //
  // This is also the process's completion time, so its comparison bookkeeping is now final: etime
  // is set here, and ctime/first_run_time/rtime/wtime were already being maintained by allocproc(),
  // the scheduler and update_times(). With SCHED_STATS on, print all of it now while p is still
  // valid; freeproc() only runs later, once the parent has reaped this zombie.
  p->etime = ticks;
#ifdef SCHED_STATS
  printk("SCHEDSTAT pid=%d name=%s ctime=%d first_run=%d etime=%d rtime=%d wtime=%d\n", p->pid,
         p->name, p->ctime, p->first_run_time, p->etime, p->rtime, p->wtime);
#endif

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++) {
      if (pp->parent == p) {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE) {
          // Found one.
          pid = pp->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); //DOC: wait-sleep
  }
}

// Records the tick a process was first handed the CPU, the moment its response time is measured
// from. Called right after a scheduler sets a process to RUNNING, with p->lock already held. Only
// the first call for a process has any effect, because after that first_run_time is no longer -1.
static void
note_dispatch(struct proc *p)
{
  if (p->first_run_time == -1)
    p->first_run_time = ticks;
}

// Scheduler comparison accounting, run once per tick for every process regardless of which
// scheduler is compiled in, so RR, FCFS and MLFQ are all measured the same way on the same
// workload. A process accumulates one more tick of rtime while RUNNING and one more tick of wtime
// while RUNNABLE (xv6's ready-queue state); SLEEPING, ZOMBIE and UNUSED processes accumulate
// neither, since they are not competing for the CPU. Called from clockintr() in trap.c, which
// already only runs this once per tick for the whole system (on CPU 0).
//
// Under MLFQ with SCHED_STATS on, this is also where each process's current queue is logged, one
// QLOG line per process per tick, which is the raw data the 2.3.2 timeline plot is built from.
void
update_times(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == RUNNING) {
      p->rtime++;
    } else if (p->state == RUNNABLE) {
      p->wtime++;
    }
#if defined(USE_MLFQ) && defined(SCHED_STATS)
    if (p->state == RUNNING || p->state == RUNNABLE)
      printk("QLOG tick=%d pid=%d queue=%d\n", ticks, p->pid, p->queue);
#endif
    release(&p->lock);
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    int found = 0;
#ifdef USE_MLFQ
    // MLFQ strict priority selection. The original loop runs processes in the order they sit in the
    // process table, which ignores priority. Instead, we first look at every process and remember
    // just one: the RUNNABLE process in the lowest-numbered (highest-priority) queue, and if several
    // are in that queue, the one with the smallest arrival stamp, i.e. the front of the queue. Only
    // that process is run. When it gives the CPU back, the loop starts again and the whole choice is
    // made again from scratch, so the highest non-empty queue always wins.
    //
    // This is also how preemption at tick boundaries works: on every timer tick trap.c calls
    // mlfq_tick(), which makes the running process give up the CPU if its slice is used up or if a
    // process is waiting in a higher queue. Control then comes back here, and the higher-queue
    // process is chosen, so a lower-queue process loses the CPU at the end of the current tick.
    //
    // Between the scan and running the winner we let go of its lock, so another CPU could start
    // running it first. We check it is still RUNNABLE before switching to it; if not, we just loop
    // and choose again. found is still set in that case, so this CPU does not go idle while there
    // is work to do.
    struct proc *best = 0;
    int best_queue = 0;
    uint64 best_time = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE &&
          (best == 0 || p->queue < best_queue ||
           (p->queue == best_queue && p->enq_time < best_time))) {
        best = p;
        best_queue = p->queue;
        best_time = p->enq_time;
      }
      release(&p->lock);
    }
    if (best != 0) {
      found = 1;
      acquire(&best->lock);
      if (best->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        best->state = RUNNING;
        note_dispatch(best);
        c->proc = best;
        swtch(&c->context, &best->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&best->lock);
    }
#elif defined(USE_FCFS)
    // FCFS selection. A lower pid means the process was created earlier (see nextpid in
    // allocproc()), so "earliest arrival" is simply "smallest pid". We scan every process and
    // remember the RUNNABLE one with the smallest pid, the same two-pass pattern used for MLFQ
    // above: find the winner while holding only one p->lock at a time, then switch to it.
    //
    // Nothing here changes how often a process gives up the CPU: trap.c still calls yield() on
    // every timer tick for this build, exactly as it does for plain RR. What changes is who gets
    // picked afterwards. When a process yields it goes back to RUNNABLE, and if it still has the
    // smallest pid among RUNNABLE processes, this scan picks it again immediately. So in practice
    // the earliest-arrived process keeps winning every tick and runs to completion, and only once
    // it exits or sleeps does the next-earliest process get a turn, which is the FCFS behaviour
    // the scheduler is supposed to have.
    struct proc *best = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE && (best == 0 || p->pid < best->pid)) {
        best = p;
      }
      release(&p->lock);
    }
    if (best != 0) {
      found = 1;
      acquire(&best->lock);
      if (best->state == RUNNABLE) {
        best->state = RUNNING;
        note_dispatch(best);
        c->proc = best;
        swtch(&c->context, &best->context);
        c->proc = 0;
      }
      release(&best->lock);
    }
#else
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        note_dispatch(p);
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
#endif
    if (found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  // A process that is preempted on a timer tick goes back into its queue here. It must go to the
  // end of the queue with a new stamp. If it kept its old, small stamp it would always look like the
  // front of the queue, and the scheduler would pick it again every time, so other processes in the
  // same queue would never run.
#ifdef USE_MLFQ
  mlfq_enqueue(p);
#endif
  sched();
  release(&p->lock);
}

#ifdef USE_MLFQ
// Time-slice accounting, run on every timer tick for the process that is currently on this CPU.
//
// First the tick is charged: ticks_used goes up by one. If that means the process has now used its
// entire slice for its queue (1, 4, 8 or 16 ticks), it is preempted and moved to the next lower
// queue. A process that is already in queue 3 has nowhere lower to go, so it stays in queue 3. In
// both cases ticks_used goes back to 0, because a new slice starts, and yield() gives up the CPU
// and puts the process at the end of its (new) queue.
//
// If the slice is not used up yet, the process normally keeps the CPU, which is what lets it run
// for its full 4, 8 or 16 ticks in one go. The one exception is the strict priority rule: if some
// RUNNABLE process is sitting in a higher-priority queue, the running process must give up the CPU
// at this tick boundary. It keeps its queue and the ticks it has already used, so it continues with
// the rest of its slice later.
//
// We let go of our own lock before looking at the other processes, because holding two process
// locks at the same time can deadlock when another CPU grabs them in the opposite order.
void
mlfq_tick(void)
{
  struct proc *p = myproc();
  struct proc *q;
  int myqueue;

  acquire(&p->lock);
  p->ticks_used++;
  if (p->ticks_used >= time_slice[p->queue]) {
    // Queue 3 is round-robin. A process in queue 3 that uses its whole 16-tick slice cannot go any
    // lower, so it stays in queue 3, and yield() below puts it at the tail of queue 3. Every other
    // queue-3 process is now ahead of it, so each of them gets a full turn before it runs again.
    if (p->queue < 3)
      p->queue++;
    p->ticks_used = 0;
    release(&p->lock);
    yield();
    return;
  }
  myqueue = p->queue;
  release(&p->lock);

  for (q = proc; q < &proc[NPROC]; q++) {
    if (q == p)
      continue;
    acquire(&q->lock);
    int waiting = (q->state == RUNNABLE && q->queue < myqueue);
    release(&q->lock);
    if (waiting) {
      yield();
      return;
    }
  }
}

// Priority boost, run every 48 ticks from clockintr() in trap.c. Without it, a steady stream of
// high-priority processes could stop processes in the lower queues from ever running (starvation).
// Every process in the system goes back to queue 0 with a fresh time slice: RUNNABLE ones waiting
// in a queue, RUNNING ones currently on a CPU, and SLEEPING ones, so that they also wake up in
// queue 0. Only UNUSED slots are skipped, because they are not processes. Arrival stamps are not
// changed, so the processes keep their existing order relative to each other inside queue 0.
void
mlfq_boost(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state != UNUSED) {
      p->queue = 0;
      p->ticks_used = 0;
    }
    release(&p->lock);
  }
}
#endif

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  // Voluntary yield under MLFQ. A process that goes to sleep (waiting for I/O, a pipe, a child, a
  // timer, ...) is giving up the CPU by itself before its time slice ran out. Because it is now
  // SLEEPING and not RUNNABLE, it has left the queues and the scheduler cannot pick it. Its queue
  // number is deliberately left alone, so when it wakes up it goes back into the same queue. The
  // part of the slice it already used is dropped (ticks_used goes back to 0): that slice ended when
  // it chose to give up the CPU, and it starts a new full slice for its queue when it runs again.
#ifdef USE_MLFQ
  p->ticks_used = 0;
#endif

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    if (p != myproc()) {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        // The process is RUNNABLE again, so it rejoins the queues. Its queue number was not changed
        // while it slept, so it goes back into the same queue it was in when it went to sleep (or
        // queue 0, if a priority boost happened in the meantime). mlfq_enqueue gives it a fresh
        // stamp, which puts it at the tail of that queue instead of letting it cut in front with its
        // old stamp.
#ifdef USE_MLFQ
        mlfq_enqueue(p);
#endif
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->killed = 1;
      if (p->state == SLEEPING) {
        // Wake process from sleep().
        p->state = RUNNABLE;
        // Killing a sleeping process wakes it up so it can exit, which is another way of rejoining a
        // queue, so it also goes to the end of its queue with a fresh stamp.
#ifdef USE_MLFQ
        mlfq_enqueue(p);
#endif
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
//
// The first line says which scheduler this kernel was built with and the current tick count, so
// one Ctrl+P is enough to tell an RR kernel from an MLFQ kernel. Under MLFQ it also shows how many
// ticks have passed since the last priority boost. The boost runs whenever ticks is a multiple of
// 48, so that number is simply ticks % 48, and it drops back to 0 right after each boost.
//
// Then there is one line per process: PID, name and state, followed under MLFQ by the scheduler
// bookkeeping for that process:
//   queue  the priority level it is in, 0 (highest) to 3 (lowest)
//   slice  ticks used so far in its current slice, out of the slice length for that queue
//   stamp  its arrival stamp; inside one queue, the smallest stamp is the front of the queue
//
// Pressing Ctrl+P a few times while a test runs shows the rules in action: a CPU-bound process
// walks down the queues as its slices run out, an I/O-bound process stays in a high queue, and
// after each boost (since boost goes back to a small number) every process is in queue 0 again.
// printk has no column widths, so every value is labeled instead of lined up in columns.
void
procdump(void)
{
  static char *states[] = {
    // clang-format off
    [UNUSED]    "unused",
    [USED]      "used",
    [SLEEPING]  "sleep ",
    [RUNNABLE]  "runble",
    [RUNNING]   "run   ",
    [ZOMBIE]    "zombie"
    // clang-format on
  };
  struct proc *p;
  char *state;

  printk("\n");
#ifdef USE_MLFQ
  printk("scheduler=MLFQ ticks=%u since_boost=%u/48\n", ticks, ticks % 48);
#else
  printk("scheduler=RR ticks=%u\n", ticks);
#endif
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printk("pid=%d name=%s state=%s", p->pid, p->name, state);
#ifdef USE_MLFQ
    printk(" queue=%d slice=%d/%d stamp=%lu", p->queue, p->ticks_used, time_slice[p->queue],
           p->enq_time);
#endif
    printk("\n");
  }
}
