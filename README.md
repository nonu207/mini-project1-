# Mini Project

This repository has two parts: **C-Shell** (`c-shell/`), a custom shell, and
**xv6-riscv** (`xv6-riscv/`), a modified xv6 kernel with FCFS and MLFQ
schedulers added alongside the default round-robin one (see
`report_xv6/REPORT.md` for the write-up).

# C-Shell (Mini Project)

C-Shell is a custom, lightweight shell written in C that parses and executes user commands. It implements a fully-featured lexer and a right-linear grammar validator, supporting advanced command-line features such as pipes, redirections, background execution, and quoting.

## Features

* **Custom Prompt:** Displays the current user, system name, and current working directory relative to the home directory.
* **Lexer & Grammar Validator:** Robust tokenization using maximal munch principles. Understands single quotes (literal), double quotes (escaped characters), and unquoted escapes. Rejects invalid grammar dynamically based on a right-linear grammar specification.
* **Pipes & Redirections:** Supports `|`, `<`, `>`, and `>>` for composing complex command pipelines.
* **Background Jobs:** Supports running commands in the background using `&`.
* **Sequential Execution:** Supports chaining multiple commands using `;`.

### Built-in Commands

1. **`change_dir` (cd):** Navigates the filesystem.
2. **`peek`:** Reads standard input or files and outputs them (supports `-r` to reverse output).
3. **`locate`:** Locates the binary of a given command (similar to `which`).
4. **`reveal`:** Lists files and directories in a specified path (similar to `ls`).
5. **`activities`:** Lists the background and stopped jobs the shell is tracking.
6. **`resume`:** Continues a stopped job in the foreground or background.
7. **`ping <target> <signal>`:** Sends `signal % 64` to a tracked pid, or to every process in a job with `%job`.
8. **`spy [pid]`:** Lists a process's open files (cwd, txt, mem, numeric fds) from `/proc`. Linux only.
9. **`snoop command [args...]` / `snoop -p pid`:** Traces a process's system calls with `ptrace` and, when it exits, prints each syscall's call count and total time. With `-p`, Ctrl-C detaches and prints the summary so far. Linux only.
10. **Execution:** Can execute external system commands seamlessly.

## Running the Shell (Linux, recommended)

The shell targets Linux: it is graded with GCC's strict POSIX flags, and `spy`
reads `/proc`, which only Linux has. On macOS, run everything inside a Linux
container with the included `c-shell/linux.sh` script.

### One-time setup

Install and start Docker Desktop entirely from the terminal (no clicking
through the app):

```bash
brew install --cask docker-desktop
docker desktop start        # starts the engine; add -d to not block until it's ready
```

Check it's up any time with `docker desktop status`, and stop it with
`docker desktop stop`. It needs to be started again after a reboot (there's
no need to reinstall). If you'd rather use the app icon instead, that works
too — install the same cask, then open it from Applications or run
`open -a Docker`.

### Commands

From the `c-shell` folder:

```bash
cd c-shell
./linux.sh           # compile in Linux, then open the shell (Ctrl-D to exit)
./linux.sh build     # only compile: exact assignment flags, plus an extra -O2 warning pass
./linux.sh bash      # a Linux bash prompt in /build with shell.out already compiled
```

Every run compiles a fresh copy of your current code, so after editing a file
just run `./linux.sh` again. Your source is mounted read-only and copied into
the container: nothing on your machine is modified.

The first run builds a small image called `cshell-linux` from `c-shell/Dockerfile`
(GCC 14, the first release that accepts `-std=c23`, plus `lsof`, `strace`, `ps`
and `script` for the tests). Later runs reuse it, and it is rebuilt
automatically if the `Dockerfile` changes.

### Before submitting

```bash
./linux.sh build     # must print "exact flags: OK"
./linux.sh test      # both suites must end with failed=0
```

### Troubleshooting

| Problem | Fix |
|---|---|
| `Docker is not running` | Run `docker desktop start` and wait for it to finish starting |
| `permission denied: ./linux.sh` | `chmod +x linux.sh` |
| You changed the `Dockerfile` | `docker rmi cshell-linux`, then run `./linux.sh` again |
| A test fails once | Re-run it with nothing else busy; the tests use short fixed waits |

## Running natively (without Docker)

On a Linux machine with GCC 14 or newer, or for a quick check on macOS:

```bash
cd c-shell
make clean all
./shell.out
```

On macOS `spy` prints `spy: /proc is not available on this system`, `snoop`
prints `snoop: not supported on this system`, and signal
numbers differ from Linux (stop is 17 and continue is 19 on macOS; 19 and 18 on
Linux; see `kill -l`).

## Structure

* `c-shell/src/` - Source code (`main.c`, `lexer.c`, `exec.c`, `ping.c`, `spy.c`, `snoop.c`, etc.)
* `c-shell/include/` - Header files (`lexer.h`, `prompt.h`, `ping.h`, `spy.h`, `snoop.h`, etc.)
  * `syscall_table.h` - Syscall number → name table for `snoop`, taking numbers from the kernel headers of whichever architecture compiles it
* `c-shell/tests/` - Test scripts for `ping`, `spy` and `snoop` (plus `snoop_target.c`, a program with a known syscall profile), run by `./linux.sh test`
* `c-shell/Dockerfile`, `c-shell/linux.sh` - The Linux build and run environment

## Running xv6

xv6 needs a RISC-V cross-compiler and `qemu-system-riscv64` (>= 7.2). On
macOS with Homebrew:

```bash
brew install riscv-software-src/riscv/riscv-tools qemu
# or, if that tap is unavailable:
brew tap riscv-software-src/riscv
brew install riscv-tools qemu
```

On Linux (Debian/Ubuntu):

```bash
sudo apt install gcc-riscv64-unknown-elf qemu-system-misc
```

The Makefile auto-detects the toolchain prefix
(`riscv64-unknown-elf-` or `riscv64-unknown-linux-gnu-`); if neither is on
your `PATH`, set `TOOLPREFIX` explicitly.

### Commands

From the `xv6-riscv` folder:

```bash
cd xv6-riscv
make clean; make qemu                      # default scheduler: round-robin (RR)
make clean; make qemu SCHEDULER=FCFS       # first-come-first-served (FIFO)
make clean; make qemu SCHEDULER=MLFQ       # multi-level feedback queue
```

`make qemu` builds the kernel and file system, then boots them in QEMU with
the xv6 shell attached to your terminal. Press `Ctrl-a x` to quit QEMU.

### `Ctrl-p`: the process dump

Pressing `Ctrl-p` at any time (no need to press Enter) asks the running
kernel to print a snapshot of every process to the console, via `procdump()`
in `kernel/proc.c`. It's not a shell command — it works even if xv6 looks
stuck, since it's read directly by the kernel's console driver. The first
line names the scheduler the kernel was built with, so one press is enough
to tell an RR/FCFS build from an MLFQ build apart:

```
$ (press Ctrl-p)
scheduler=RR ticks=143
pid=1 name=init state=sleep
pid=2 name=sh state=sleep
pid=3 name=schedulertest state=run
```

Under `SCHEDULER=MLFQ`, the first line also shows ticks since the last
priority boost, and each process's line adds its queue (0 = highest, 3 =
lowest), ticks used so far in its current time slice, and its arrival
stamp (smaller = closer to the front of its queue):

```
$ (press Ctrl-p)
scheduler=MLFQ ticks=143 since_boost=47/48
pid=1 name=init state=sleep queue=0 slice=0/1 stamp=1
pid=3 name=schedulertest state=run queue=2 slice=5/8 stamp=214
```

Use it while a test like `schedulertest` is running, pressing it a few
times in a row, to watch processes move down through the queues as their
slices run out and jump back to queue 0 at the next boost.

Add `STATS=1` to either build to also log per-process timing (and, under
MLFQ, a per-tick queue trace) used for the scheduler comparison in
`report_xv6/REPORT.md`:

```bash
make clean; make qemu SCHEDULER=MLFQ STATS=1
```

`make clean` is required when switching `SCHEDULER`/`STATS` values, since
they are compiled in via `-D` flags rather than read at runtime.

### Structure

* `xv6-riscv/kernel/` - Kernel source, including `proc.c`/`proc.h` (the MLFQ
  scheduler, priority boosting, and `procdump`) and `trap.c` (per-tick
  scheduling hooks)
* `xv6-riscv/user/` - User-space programs, including `schedulertest.c`, a
  fixed CPU-bound/I/O-bound workload used to exercise and compare schedulers
* `report_xv6/` - The scheduler comparison report, its timeline plot, and the
  script that generates it from `STATS=1` output
