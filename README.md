# Mini Project 1 — CS3.301 Operating Systems and Networks

This repository contains the implementation for **Mini Project 1** of CS3.301 Operating Systems and Networks.

## Repository Structure

```
mini-project1/
├── README.md                          # Root documentation
├── Mini Project 1 — CS3.301.pdf      # Specification document
├── c-shell/                           # Part 1: C-Shell Implementation
│   ├── Makefile                       # Build system for C-Shell (produces shell.out)
│   ├── include/                       # C-Shell Header files
│   │   ├── shell.h                    # Global definitions and shell context
│   │   ├── prompt.h                   # Shell prompt formatting
│   │   ├── lexer.h                    # Lexical tokens and tokenizer
│   │   ├── parser.h                   # Grammar parser & command structure
│   │   ├── intrinsics.h               # Builtins: hop, reveal, peek, locate
│   │   ├── execute.h                  # Execution, redirection, & piping
│   │   ├── jobs.h                     # Job control, background jobs, activities, resume, ping
│   │   └── fun.h                      # Diagnostic builtins: spy, snoop
│   └── src/                           # C-Shell Source code
│       ├── main.c                     # REPL loop entry point
│       ├── prompt.c                   # Prompt implementation
│       ├── lexer.c                    # Lexer implementation (maximal munch)
│       ├── parser.c                   # Grammar parser implementation
│       ├── intrinsics.c               # Builtins implementation
│       ├── execute.c                  # Execution & pipeline logic
│       ├── jobs.c                     # Job tracking & terminal control
│       └── fun.c                      # Proc & ptrace inspection
└── xv6/                               # Part 2: xv6 MLFQ Scheduler
    ├── Makefile                       # xv6 Makefile supporting SCHEDULER=MLFQ
    ├── kernel/                        # xv6 Kernel source files
    ├── user/                          # xv6 User programs (includes schedulertest.c)
    ├── mkfs/                          # File system generator
    └── report.md                      # MLFQ analysis & scheduler comparison report
```

## Quick Start

### Building & Running C-Shell

```bash
cd c-shell
make all
./shell.out
```

To clean build artifacts:
```bash
make clean
```

### Building xv6 with MLFQ Scheduler

```bash
cd xv6
# Build with default Round Robin scheduler
make qemu

# Build with MLFQ scheduler
make qemu SCHEDULER=MLFQ
```
