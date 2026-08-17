# xv6 Multi-Level Feedback Queue (MLFQ) Scheduler Report

## 1. Implementation Summary

### Makefile / SCHEDULER Macro
The root `Makefile` was extended to accept `SCHEDULER=MLFQ` as a command-line flag during compilation. When passed, the macro `-DMLFQ` is appended to `CFLAGS`.

### `struct proc` Changes
Added scheduler bookkeeping fields to `struct proc` in `kernel/proc.h`:
- `priority`: Current queue level (0 highest to 3 lowest).
- `ticks_consumed`: Ticks spent in current queue level.
- `ticks_since_boost`: Total ticks accumulated since global priority boost.

### Queue Selection & Preemption
- **Priority Selection**: Strict priority selection schedules processes from the highest non-empty queue ($Q_0 > Q_1 > Q_2 > Q_3$).
- **Time-Slice Exhaustion**: Processes exceeding their queue time slice ($Q_0$: 1, $Q_1$: 4, $Q_2$: 8, $Q_3$: 16 ticks) are demoted to the tail of the next lower queue.
- **Priority Boosting**: Every 48 ticks, all runnable processes are reset to Queue 0 to prevent starvation.

---

## 2. Cross-Scheduler Performance Comparison

| Metric | FIFO | Round Robin (RR) | MLFQ |
| :--- | :--- | :--- | :--- |
| **Average Turnaround Time** | - | - | - |
| **Average Waiting Time** | - | - | - |
| **Average Response Time** | - | - | - |

---

## 3. MLFQ Queue Movement Analysis & Timeline Plot

*(Include timeline/scatter plot graph showing process queue movement over ticks and periodic 48-tick boosts)*

### Interpretation
- **CPU-Bound Processes**: Demoted rapidly down to Queue 3 after exhausting time slices in higher queues.
- **I/O-Bound Processes**: Remain in higher queues ($Q_0, Q_1$) due to voluntary yields before time slice exhaustion, receiving lower latency.
- **Priority Boost**: Periodic 48-tick boost resets all processes to $Q_0$, enabling aging CPU-bound processes to run again without starvation.
