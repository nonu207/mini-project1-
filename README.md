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

1. Install [Docker Desktop](https://www.docker.com/products/docker-desktop/).
2. Start it (open it from Applications, or run `open -a Docker`) and wait until it has finished starting.

### Commands

From the `c-shell` folder:

```bash
cd c-shell
./linux.sh           # compile in Linux, then open the shell (Ctrl-D to exit)
./linux.sh build     # only compile: exact assignment flags, plus an extra -O2 warning pass
./linux.sh test      # compile, then run the ping, spy and snoop test suites
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
| `Docker is not running` | Start Docker Desktop and wait for it to finish starting |
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
