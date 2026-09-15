# AI USAGE for end submission
exported markdown of chat. 
## You

Explain how Unix file descriptors work when chaining multiple processes using ⁠ pipe() ⁠ and ⁠ dup2() ⁠.

## ChatGPT

Think of a Unix pipeline like:

```bash
A | B | C
```

where:

```text
A's output → B's input → B's output → C's input
```

The key idea is that **`pipe()` creates file descriptors, and `dup2()` makes those descriptors become standard input/output (`stdin`/`stdout`) for each process.**

---

## 1. What is a file descriptor?

A file descriptor (FD) is just an integer used by a process to refer to an open file/resource.

The standard ones are:

```text
0 → stdin
1 → stdout
2 → stderr
```

For example:

```c
write(1, "hello", 5);
```

writes to standard output.

---

# 2. What does `pipe()` do?

When you call:

```c
int fd[2];
pipe(fd);
```

Unix creates a **one-way communication channel**:

```text
fd[0] → read end
fd[1] → write end
```

So conceptually:

```text
       PIPE
   ┌───────────┐
   │           │
fd[1] ───────→ fd[0]
write          read
```

If one process does:

```c
write(fd[1], "hello", 5);
```

another process can do:

```c
read(fd[0], buffer, 5);
```

and receive `"hello"`.

---

# 3. What happens after `fork()`?

This is where file descriptors become particularly useful.

Suppose:

```c
pipe(fd);
fork();
```

After `fork()`, **both parent and child have copies of the file descriptors**.

Initially:

```text
Parent                    Child
------                    -----

fd[0] ──→ pipe read       fd[0] ──→ pipe read
fd[1] ──→ pipe write      fd[1] ──→ pipe write
```

They refer to the same underlying pipe.

This allows the parent and child to communicate through it.

---

# 4. What does `dup2()` do?

Suppose:

```c
dup2(fd[1], STDOUT_FILENO);
```

Since:

```c
STDOUT_FILENO = 1
```

this means:

> Make file descriptor `1` refer to the same open file/pipe as `fd[1]`.

So instead of:

```text
stdout (1) → terminal
fd[1]      → pipe
```

you now have:

```text
stdout (1) ──→ pipe
fd[1]       ──→ pipe
```

Therefore:

```c
printf("hello\n");
```

or:

```c
write(1, ...);
```

will write into the pipe rather than to the terminal.

Similarly:

```c
dup2(fd[0], STDIN_FILENO);
```

makes:

```text
stdin (0) → pipe read end
```

So:

```c
scanf(...)
```

or:

```c
read(0, ...)
```

reads from the pipe.

---

# 5. Now consider `A | B`

We want:

```text
A
│
│ stdout
▼
PIPE
│
│ stdin
▼
B
```

We need **one pipe**:

```c
int fd[2];
pipe(fd);
```

Then we create the two processes.

```c
fork();
```

## Process A

A needs to **write into the pipe**.

So:

```c
dup2(fd[1], STDOUT_FILENO);
```

Now:

```text
A:
stdout (1)
    │
    ▼
 pipe write end
```

Therefore anything A writes to stdout goes into the pipe.

---

## Process B

B needs to **read from the pipe**.

So:

```c
dup2(fd[0], STDIN_FILENO);
```

Now:

```text
B:
stdin (0)
   │
   ▼
pipe read end
```

Therefore anything B reads from stdin comes from A.

The final setup is:

```text
             PIPE
        ┌──────────────┐
        │              │
A       │              │       B
stdout ─┼── write → read ─┼─→ stdin
        │              │
        └──────────────┘
```

This is exactly what the shell needs to implement:

```bash
A | B
```

---

# 6. What about multiple processes?

Now consider:

```bash
A | B | C
```

We need **two pipes**:

```text
        Pipe 1             Pipe 2

A ─────────────→ B ─────────────→ C
  stdout→       stdin    stdout→
```

Let's call them:

```c
int pipe1[2];
int pipe2[2];

pipe(pipe1);
pipe(pipe2);
```

Their ends are:

```text
pipe1[0] = read
pipe1[1] = write

pipe2[0] = read
pipe2[1] = write
```

---

## Process A

A only needs to write to Pipe 1.

```c
dup2(pipe1[1], STDOUT_FILENO);
```

So:

```text
A
stdout
  │
  ▼
pipe1 write
```

---

## Process B

B receives input from Pipe 1 and sends output to Pipe 2.

Therefore:

```c
dup2(pipe1[0], STDIN_FILENO);
dup2(pipe2[1], STDOUT_FILENO);
```

So B becomes:

```text
             B
             │
stdin        │        stdout
  │          │          │
  ▼          │          ▼
pipe1 read → B → pipe2 write
```

This is the important part: **B has both a redirected stdin and a redirected stdout.**

---

## Process C

C only needs to read from Pipe 2:

```c
dup2(pipe2[0], STDIN_FILENO);
```

So:

```text
pipe2 read
    │
    ▼
    C
  stdin
```

---

# 7. Complete picture

After all the `dup2()` calls:

```text
              PIPE 1
        ┌─────────────────┐
        │                 │
        ▼                 │
   ┌─────────┐       ┌─────────┐
   │    A    │       │    B    │
   │         │       │         │
   │ stdout ─┼──────→│ stdin   │
   └─────────┘       │ stdout ─┼──────→
                     └─────────┘         │
                                         │
                            PIPE 2        │
                         ┌───────────────┐│
                         │               ││
                         ▼               ▼
                    ┌─────────┐
                    │    C    │
                    │  stdin  │
                    └─────────┘
```

Or more simply:

```text
A stdout
   │
   ▼
pipe1
   │
   ▼
B stdin
   │
   │ B processes data
   ▼
B stdout
   │
   ▼
pipe2
   │
   ▼
C stdin
```

---

# 8. Why do we close file descriptors?

This is **very important** when implementing pipelines.

Suppose A is writing to Pipe 1 and B is reading from it.

We don't want A to keep the **read end** open, because A doesn't need it.

Similarly, B doesn't need the original write/read descriptors after `dup2()`.

For example, A might do:

```c
close(pipe1[0]);        // doesn't read
dup2(pipe1[1], 1);
close(pipe1[1]);        // no longer needed
exec(...);
```

Notice something interesting:

```c
dup2(pipe1[1], 1);
close(pipe1[1]);
```

is safe because after `dup2()`:

```text
FD 1 ─────→ pipe1 write
```

So closing `pipe1[1]` doesn't close the pipe itself. It only closes that **particular descriptor**.

The duplicated FD `1` remains open.

---

# 9. Why closing matters for EOF

This is one of the most important consequences.

Suppose B is doing:

```c
read(0, buffer, sizeof(buffer));
```

Eventually, B needs to know:

> "There is no more data coming."

A pipe reports EOF only when **all file descriptors referring to its write end have been closed**.

So if some process accidentally keeps the write end open:

```text
A ──→ PIPE ──→ B
       ↑
       │
   some other
   process still
   has write end
```

B may wait indefinitely for more data.

That's why proper pipeline implementations close unused pipe ends in **every process**.

---

# 10. The role of `exec()`

Usually, after setting up the file descriptors, we call:

```c
exec(...)
```

For example, for:

```bash
ls | grep txt
```

the shell roughly does:

### Process 1

```c
dup2(pipe[1], STDOUT_FILENO);
close(pipe[0]);
close(pipe[1]);

execlp("ls", "ls", NULL);
```

After `exec`, the new `ls` program inherits:

```text
stdout → pipe
```

So `ls` doesn't know or care that it's writing to a pipe.

It simply writes to stdout.

---

### Process 2

```c
dup2(pipe[0], STDIN_FILENO);
close(pipe[0]);
close(pipe[1]);

execlp("grep", "grep", "txt", NULL);
```

After `exec`:

```text
grep's stdin → pipe
```

So `grep` simply reads from stdin.

This is why Unix pipes are so elegant: **the programs don't need special pipe-related code.** The shell changes what their standard input/output point to.

---

# 11. The general rule for `A | B | C | D`

For a process somewhere in the middle:

```text
A | B | C | D
```

each middle process needs:

```text
stdin  ← previous pipe's read end
stdout → next pipe's write end
```

So:

```text
A:
    stdout → pipe1

B:
    stdin  ← pipe1
    stdout → pipe2

C:
    stdin  ← pipe2
    stdout → pipe3

D:
    stdin  ← pipe3
```

The overall structure is:

```text
       pipe1        pipe2        pipe3
A ───────────→ B ───────────→ C ───────────→ D
   stdout       stdin stdout   stdin stdout   stdin
```

---

## The simplest way to remember it

**`pipe()` creates the connection.**

```text
pipe → [read end, write end]
```

**`dup2()` connects that pipe to standard input/output.**

```text
dup2(pipe_write, 1)  → stdout goes INTO pipe
dup2(pipe_read,  0)  → stdin comes FROM pipe
```

So for a pipeline:

```text
        write end                  read end
           ↓                          ↓
A stdout ─────→ PIPE ─────→ B stdin
```

and for multiple processes:

```text
A stdout → pipe1 → B stdin
                    B stdout → pipe2 → C stdin
```

The `fork()` gives processes access to the pipe descriptors, `dup2()` puts the correct pipe end on FD 0 or 1, and `exec()` then runs the actual program with those redirected standard streams.

## You

What happens if a child process doesn't close the write end of a pipe before calling ⁠ execvp() ⁠? Why does the reader process hang indefinitely waiting for ⁠ EOF ⁠?

## ChatGPT

Because **`execvp()` does not close ordinary file descriptors**. It replaces the program's code/data, but the process keeps its open file descriptors unless they have the `FD_CLOEXEC` flag.

Consider:

```text
A | B
```

with:

```text
A → pipe → B
```

Suppose the pipe has:

```text
pipe[0] = read end
pipe[1] = write end
```

### What should happen

The writer process A does:

```c
dup2(pipe[1], STDOUT_FILENO);
close(pipe[0]);
close(pipe[1]);
execvp(...);
```

After `execvp()`, A's new program has:

```text
stdout → pipe write end
```

and no other copy of the write end.

When A eventually exits, its stdout is closed. If **no other process has the write end open**, the pipe has no writers left.

Therefore B's:

```c
read(...)
```

returns:

```text
0
```

which means **EOF**.

---

### What goes wrong if a child forgets `close(pipe[1])`?

Suppose A does:

```c
dup2(pipe[1], STDOUT_FILENO);
// forgot: close(pipe[1])

execvp(...);
```

Before `execvp()`:

```text
A:
    FD 1 ─────┐
              ├──→ pipe write end
    pipe[1] ──┘
```

There are now **two file descriptors** in A referring to the same pipe write end.

After `execvp()`:

```text
New program:
    FD 1 ───────→ pipe write end
    FD pipe[1] ─→ pipe write end
```

`execvp()` **doesn't magically clean up `pipe[1]`**.

---

### Why does B hang?

A common reader pattern is:

```c
while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
    // process data
}
```

The reader gets EOF only when:

> **all references to the pipe's write end have been closed.**

If A finishes writing but still has the inherited `pipe[1]` open, the kernel sees:

```text
        PIPE
         │
    ┌────┴────┐
    │         │
  data       write end
    │         │
    ▼         ▼
    B         A
```

B has already consumed all available data.

But the kernel thinks:

> "There is still a process that could potentially write more data."

Therefore it **cannot return EOF yet**.

So B's next:

```c
read(...)
```

blocks, waiting for either:

1. more data to be written, or
2. **all write ends to be closed**, allowing EOF.

If A's program is no longer going to write anything, B can wait **indefinitely**.

### The key distinction

EOF does **not** mean:

> "The writer has no more data right now."

It means:

> **"There are no open write descriptors for this pipe anymore."**

That's why this is important:

```c
dup2(pipe[1], STDOUT_FILENO);
close(pipe[1]);       // important
execvp(...);
```

After `dup2()`, FD 1 is already another reference to the same pipe endpoint, so the original `pipe[1]` should be closed.

For pipelines with multiple processes, **every process must close every pipe end it doesn't need**, otherwise an apparently finished pipeline can hang waiting for EOF.

## You

What file permissions (⁠ 0644 ⁠) should be passed to ⁠ open()

## ChatGPT

If you're opening/creating a file with `open()` and want **permissions `0644`**, you would write:

```c
int fd = open("file.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
```

### What does `0644` mean?

It's an **octal** permission value:

```text
0644
│││
││└── others: 4  → read
│└─── group:  4  → read
└──── owner:  6  → read + write
```

So:

```text
0644 = rw-r--r--
```

| User | Permissions | Value |
|---|---|---:|
| Owner | read + write (`rw-`) | 6 |
| Group | read (`r--`) | 4 |
| Others | read (`r--`) | 4 |

Therefore, the **owner can read and write**, while everyone else can **only read**.

### Important

The third argument (`0644`) is only relevant when `O_CREAT` is used:

```c
open("file.txt", O_WRONLY | O_CREAT, 0644);
```

If you're **not creating** the file, you don't need to provide it:

```c
open("file.txt", O_WRONLY);
```

Also, the actual permissions can be affected by the process's **umask**. For example, with a typical umask of `022`, `0644` remains `0644`.

## You

write a C function ⁠ that uses ⁠ pipe() ⁠, ⁠ fork() ⁠, and ⁠ dup2() ⁠ to run an arbitrary n-stage pipeline with input and output redirection.

## ChatGPT

Here is a reusable implementation for a pipeline like:

```text
input_file → cmd1 → cmd2 → ... → cmdN → output_file
```

Each command is represented as an argument array suitable for `execvp()`.

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

void run_pipeline(char *commands[][10], int n,
                  const char *input_file,
                  const char *output_file)
{
    int prev_fd = -1;
    pid_t pids[n];

    for (int i = 0; i < n; i++) {

        int pipefd[2];

        // Create a pipe unless this is the last command
        if (i < n - 1) {
            if (pipe(pipefd) == -1) {
                perror("pipe");
                exit(1);
            }
        }

        pids[i] = fork();

        if (pids[i] == -1) {
            perror("fork");
            exit(1);
        }

        if (pids[i] == 0) {
            // ---------- CHILD ----------

            // Input:
            // First command gets input from input_file.
            // Other commands get input from previous pipe.
            if (i == 0) {
                int fd = open(input_file, O_RDONLY);

                if (fd == -1) {
                    perror("open input");
                    exit(1);
                }

                if (dup2(fd, STDIN_FILENO) == -1) {
                    perror("dup2 input");
                    exit(1);
                }

                close(fd);
            } else {
                if (dup2(prev_fd, STDIN_FILENO) == -1) {
                    perror("dup2 stdin");
                    exit(1);
                }

                close(prev_fd);
            }

            // Output:
            // Last command writes to output_file.
            // Other commands write to the next pipe.
            if (i == n - 1) {
                int fd = open(output_file,
                              O_WRONLY | O_CREAT | O_TRUNC,
                              0644);

                if (fd == -1) {
                    perror("open output");
                    exit(1);
                }

                if (dup2(fd, STDOUT_FILENO) == -1) {
                    perror("dup2 output");
                    exit(1);
                }

                close(fd);
            } else {
                if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
                    perror("dup2 stdout");
                    exit(1);
                }

                close(pipefd[0]);
                close(pipefd[1]);
            }

            // Run the command
            execvp(commands[i][0], commands[i]);

            // Only reached if execvp fails
            perror("execvp");
            exit(1);
        }

        // ---------- PARENT ----------

        // The parent no longer needs the previous pipe.
        if (prev_fd != -1) {
            close(prev_fd);
        }

        // Save this pipe's read end for the next command.
        if (i < n - 1) {
            close(pipefd[1]);       // parent doesn't write
            prev_fd = pipefd[0];    // next child will read from this
        }
    }

    // Parent no longer needs the final read end.
    if (prev_fd != -1) {
        close(prev_fd);
    }

    // Wait for all children.
    for (int i = 0; i < n; i++) {
        waitpid(pids[i], NULL, 0);
    }
}
```

### Example

For:

```bash
cat input.txt | grep hello | wc -l > output.txt
```

you could call:

```c
char *commands[][10] = {
    {"cat", NULL},
    {"grep", "hello", NULL},
    {"wc", "-l", NULL}
};

run_pipeline(commands, 3, "input.txt", "output.txt");
```

The resulting connections are:

```text
input.txt
    │
    ▼
 stdin of cat
    │
    │ stdout
    ▼
 ┌────────┐
 │ pipe 1 │
 └────────┘
    │
    ▼
 stdin of grep
    │
    │ stdout
    ▼
 ┌────────┐
 │ pipe 2 │
 └────────┘
    │
    ▼
 stdin of wc
    │
    │ stdout
    ▼
output.txt
```

### The important `dup2()` logic

For command `i`:

```text
i == 0
    stdin  ← input file
    stdout → pipe 1

0 < i < n-1
    stdin  ← previous pipe
    stdout → next pipe

i == n-1
    stdin  ← previous pipe
    stdout → output file
```

So for a middle process such as `grep`:

```c
dup2(prev_fd, STDIN_FILENO);
dup2(pipefd[1], STDOUT_FILENO);
```

After that, `grep` doesn't know it's part of a pipeline. It simply reads from FD `0` and writes to FD `1`.

**One particularly important detail:** after `dup2()`, close the original descriptor. Otherwise an inherited pipe write end can remain open across `execvp()`, preventing the downstream reader from ever seeing EOF.

## You

how can I structure a pipeline execution loop in C so that parent process creates ⁠ n-1 ⁠ pipes, forks ⁠ n ⁠ children, correctly redirects ⁠ stdin ⁠/⁠ stdout ⁠ for each child

## ChatGPT

The cleanest way is to think of an **n-stage pipeline as n children connected by n−1 pipes**.

For:

```text
cmd1 | cmd2 | cmd3 | ... | cmdn
```

you want:

```text
pipe 0          pipe 1
   ↓               ↓
cmd1 → [pipe] → cmd2 → [pipe] → cmd3 → ... → cmdn
```

### 1. The basic loop structure

A good pattern is:

```c
int prev_fd = -1;

for (int i = 0; i < n; i++) {
    int pipefd[2];

    // Create a pipe for every child except the last
    if (i < n - 1) {
        pipe(pipefd);
    }

    pid_t pid = fork();

    if (pid == 0) {
        // ---------- CHILD ----------

        // Input
        if (i == 0) {
            // First child:
            // stdin stays as normal stdin
            // OR redirect it from an input file
        } else {
            // Read from previous pipe
            dup2(prev_fd, STDIN_FILENO);
        }

        // Output
        if (i == n - 1) {
            // Last child:
            // stdout stays as normal stdout
            // OR redirect it to an output file
        } else {
            // Write to current pipe
            dup2(pipefd[1], STDOUT_FILENO);
        }

        // Close unused descriptors
        if (prev_fd != -1)
            close(prev_fd);

        if (i < n - 1) {
            close(pipefd[0]);
            close(pipefd[1]);
        }

        execvp(...);
        perror("execvp");
        exit(1);
    }

    // ---------- PARENT ----------

    // Parent no longer needs the previous pipe
    if (prev_fd != -1)
        close(prev_fd);

    // Parent doesn't write to the current pipe
    if (i < n - 1) {
        close(pipefd[1]);

        // Save read end for the next child
        prev_fd = pipefd[0];
    }
}
```

Then, after the loop:

```c
if (prev_fd != -1)
    close(prev_fd);

for (int i = 0; i < n; i++)
    wait(NULL);
```

---

## 2. Why `prev_fd` is useful

You don't need to keep all `n-1` pipes around.

Instead, maintain:

```c
int prev_fd = -1;
```

It represents:

> **the read end of the previous pipe**

For example, with:

```text
A | B | C
```

### Iteration 0 — A

Create:

```text
pipe0 = [read0, write0]
```

Child A:

```c
dup2(write0, STDOUT_FILENO);
```

So:

```text
A stdout → pipe0
```

Parent saves:

```c
prev_fd = read0;
```

---

### Iteration 1 — B

Create:

```text
pipe1 = [read1, write1]
```

Child B does:

```c
dup2(prev_fd, STDIN_FILENO);
dup2(write1, STDOUT_FILENO);
```

Therefore:

```text
pipe0 → B → pipe1
```

Then the parent closes the old `prev_fd` and saves:

```c
prev_fd = read1;
```

---

### Iteration 2 — C

No new pipe is needed because C is the last process.

Child C does:

```c
dup2(prev_fd, STDIN_FILENO);
```

So:

```text
pipe1 → C
```

The final structure is:

```text
       pipe0             pipe1
       ┌───┐             ┌───┐
A ────→│   │────→ B ────→│   │────→ C
       └───┘             └───┘
          ↑                 ↑
       prev_fd           prev_fd
```

---

## 3. Adding input/output redirection

If you want:

```bash
input.txt → A | B | C → output.txt
```

then modify the two boundary cases.

### First child

Instead of leaving stdin alone:

```c
int fd = open("input.txt", O_RDONLY);
dup2(fd, STDIN_FILENO);
close(fd);
```

So:

```text
input.txt → A
```

### Last child

Instead of leaving stdout alone:

```c
int fd = open("output.txt",
              O_WRONLY | O_CREAT | O_TRUNC,
              0644);

dup2(fd, STDOUT_FILENO);
close(fd);
```

So:

```text
C → output.txt
```

The middle children don't care about the files.

---

## 4. The complete mental model

For each child, there are only **three cases** to remember:

```text
             stdin                 stdout
             ─────                 ──────

FIRST        input file            pipe
             OR normal stdin       ↓

MIDDLE       previous pipe         next pipe
             ↓                     ↓

LAST         previous pipe         output file
             ↓                     OR normal stdout
```

So the core logic is essentially:

```c
if (i > 0)
    dup2(prev_fd, STDIN_FILENO);

if (i < n - 1)
    dup2(pipefd[1], STDOUT_FILENO);
```

with special handling for:

```c
i == 0       // input redirection
i == n - 1   // output redirection
```

---

## 5. Why the parent must close pipe ends

This part is easy to get wrong.

After forking a child that writes to a pipe, the **parent must close its copy of the write end**:

```c
close(pipefd[1]);
```

Otherwise the parent itself still has a writer open.

Then when the final reader tries:

```c
read(...)
```

it may never receive EOF because the kernel still sees an open write descriptor somewhere.

The general rule is:

```text
Child:
    keep only the pipe ends needed for stdin/stdout
    close everything else

Parent:
    close pipe ends as soon as it no longer needs them
    keep only the read end needed for the next iteration
```

This gives you an efficient pipeline loop without needing an array of all pipes.

## You

Help me implement a check during shell parsing that rejects invalid redirection syntax like ⁠ cat < ⁠ (missing file), ⁠ cat > file1 > file2 ⁠ (multiple outputs), or ⁠ | grep ⁠ pipe with missing left operand

## ChatGPT

Yes. The easiest approach is to make redirection and pipe validation a **separate parsing/validation pass before you create any pipes or fork any children**.

Assume your shell has already tokenized input into something like:

```c
char *tokens[] = {"cat", "<", NULL};
```

or:

```c
char *tokens[] = {"cat", ">", "file1", ">", "file2", NULL};
```

You can validate the syntax first.

### Basic rules

Reject:

```text
cat <                 // < has no filename
cat >                 // > has no filename
cat < < input.txt     // < followed by another operator
cat > > output.txt    // > followed by another operator
cat > file1 > file2   // more than one output redirection
| grep                // pipe has no left command
cat |                 // pipe has no right command
cat || grep           // empty command between pipes
```

Here's a compact validator:

:::writing{variant="document" id="58321" title="Shell Syntax Validation Function"}
```c
#include <stdio.h>
#include <string.h>

#define MAX_TOKENS 100

/*
 * Returns 0 if the command is valid.
 * Returns -1 if the command has invalid syntax.
 *
 * tokens must be NULL-terminated.
 */
int validate_syntax(char *tokens[])
{
    int input_redirection = 0;
    int output_redirection = 0;
    int command_has_token = 0;

    for (int i = 0; tokens[i] != NULL; i++) {

        char *tok = tokens[i];

        /* ---------------- PIPE ---------------- */
        if (strcmp(tok, "|") == 0) {

            /*
             * A pipe cannot be the first token,
             * and cannot immediately follow another pipe.
             */
            if (!command_has_token) {
                fprintf(stderr,
                        "syntax error: pipe with missing left operand\n");
                return -1;
            }

            /*
             * The next token must exist and must not
             * itself be a pipe.
             */
            if (tokens[i + 1] == NULL ||
                strcmp(tokens[i + 1], "|") == 0) {

                fprintf(stderr,
                        "syntax error: pipe with missing right operand\n");
                return -1;
            }

            /*
             * Start checking the next command.
             */
            command_has_token = 0;
            continue;
        }

        /* ------------- INPUT REDIRECTION ------------- */
        if (strcmp(tok, "<") == 0) {

            /*
             * < must be followed by a filename.
             */
            if (tokens[i + 1] == NULL ||
                strcmp(tokens[i + 1], "|") == 0 ||
                strcmp(tokens[i + 1], "<") == 0 ||
                strcmp(tokens[i + 1], ">") == 0) {

                fprintf(stderr,
                        "syntax error: missing input file after '<'\n");
                return -1;
            }

            input_redirection++;

            if (input_redirection > 1) {
                fprintf(stderr,
                        "syntax error: multiple input redirections\n");
                return -1;
            }

            /*
             * Skip the filename because we have already
             * validated it.
             */
            i++;
            continue;
        }

        /* ------------- OUTPUT REDIRECTION ------------- */
        if (strcmp(tok, ">") == 0) {

            /*
             * > must be followed by a filename.
             */
            if (tokens[i + 1] == NULL ||
                strcmp(tokens[i + 1], "|") == 0 ||
                strcmp(tokens[i + 1], "<") == 0 ||
                strcmp(tokens[i + 1], ">") == 0) {

                fprintf(stderr,
                        "syntax error: missing output file after '>'\n");
                return -1;
            }

            output_redirection++;

            if (output_redirection > 1) {
                fprintf(stderr,
                        "syntax error: multiple output redirections\n");
                return -1;
            }

            /*
             * Skip the filename.
             */
            i++;
            continue;
        }

        /* ---------------- NORMAL TOKEN ---------------- */

        /*
         * This is a command/argument.
         */
        command_has_token = 1;
    }

    /*
     * Input cannot end immediately after a pipe.
     */
    if (!command_has_token) {
        fprintf(stderr,
                "syntax error: pipe with missing right operand\n");
        return -1;
    }

    return 0;
}
```
:::

### Using it in your shell

The important part is **where** you call it.

Do this:

```c
char *tokens[MAX_TOKENS];

tokenize(line, tokens);

if (validate_syntax(tokens) < 0) {
    // Don't create pipes.
    // Don't fork.
    // Don't exec.
    continue;
}

/* Syntax is valid → now execute the pipeline */
execute_pipeline(tokens);
```

So your shell's flow becomes:

```text
                 user enters command
                         │
                         ▼
                    tokenize()
                         │
                         ▼
                  validate_syntax()
                    /          \
                 invalid        valid
                   │              │
                   ▼              ▼
             print error    parse pipeline
                                  │
                                  ▼
                             create pipes
                                  │
                                  ▼
                               fork()
                                  │
                                  ▼
                               dup2()
                                  │
                                  ▼
                              execvp()
```

### One thing to watch for

If your tokenizer currently treats `<` and `>` as ordinary characters, you'll need to make sure:

```text
cat<input.txt
cat >file.txt
```

are tokenized appropriately as:

```text
cat
<
input.txt
```

and:

```text
cat
>
file.txt
```

respectively.

Also, the validator above assumes **one input and one output redirection for the entire pipeline**. If your assignment has more specific rules—for example, allowing `<` only on the first command and `>` only on the last command—you should enforce that during parsing too.

## You

How does shell sequential execution (⁠ ; ⁠) differ from background execution (⁠ & ⁠) in terms of process waiting

## ChatGPT

The main difference is **whether the shell waits for the child process to finish before continuing**.

### `;` — sequential execution

For:

```bash
sleep 5 ; echo hello
```

The shell:

```text
Shell
  │
  ├── fork() → sleep 5
  │
  ├── waitpid() ← waits 5 seconds
  │
  └── echo hello
```

So `echo hello` **doesn't start until `sleep 5` finishes**.

Conceptually:

```c
pid = fork();

if (pid == 0) {
    execvp(...);
} else {
    waitpid(pid, NULL, 0);   // wait for command
}
```

Then the shell parses/executes the next command.

---

### `&` — background execution

For:

```bash
sleep 5 & echo hello
```

The shell:

```text
Shell
  │
  ├── fork() → sleep 5
  │              │
  │              └── runs independently
  │
  └── immediately executes echo hello
```

The shell **does not wait immediately** for the background process.

Conceptually:

```c
pid = fork();

if (pid == 0) {
    execvp(...);
} else {
    // Don't wait here
    // Continue processing commands
}
```

So `echo hello` can execute immediately while `sleep` continues running.

---

### Side-by-side

| | `;` | `&` |
|---|---|---|
| Starts child? | Yes | Yes |
| Shell waits immediately? | **Yes** | **No** |
| Next command starts | After previous finishes | Immediately |
| Example | `A ; B` | `A & B` |
| Processes overlap? | Usually no | **Yes** |

For example:

```bash
sleep 5 ; echo A
```

takes roughly **5 seconds** before `A` appears.

Whereas:

```bash
sleep 5 & echo A
```

prints `A` almost immediately while `sleep` continues in the background.

### One important shell detail

A background process still eventually needs to be reaped with `waitpid()` to avoid becoming a zombie. A real shell typically does this asynchronously, often using `SIGCHLD` handling or periodically calling `waitpid(..., WNOHANG)`.

So the distinction isn't that `&` means **"never wait"**. It means:

> **Don't block the shell waiting for this command before continuing with the next command.**

## You

Why do background processes become zombie processes if the parent shell doesn't call ⁠ waitpid()

## ChatGPT

Because when a child process **finishes**, Unix does not immediately remove all information about it. The kernel keeps a small record so that the parent can retrieve the child's exit status using `wait()` or `waitpid()`.

That's what a **zombie process** is.

### Example with `&`

Suppose your shell executes:

```bash
sleep 5 &
```

The shell does roughly:

```text
Shell
  │
  └── fork()
       │
       └── Child: sleep 5
```

Because it's a background process, the shell **doesn't immediately call `waitpid()`**.

After 5 seconds:

```text
Child: sleep 5
        │
        ▼
      exits
```

The child is no longer executing, but the kernel keeps a small process-table entry containing things such as:

```text
PID
exit status
resource/accounting information
```

The state becomes:

```text
Child → ZOMBIE
```

The shell still has to acknowledge/reap it:

```c
waitpid(child_pid, &status, 0);
```

After `waitpid()`:

```text
Zombie → completely removed
```

### Why does Unix do this?

Imagine the child does:

```c
exit(42);
```

The parent might want to know:

> "Did my child succeed or fail? What was its exit status?"

The kernel therefore keeps the exit information until the parent collects it.

```text
Child:
    exit(42)
       │
       ▼
Kernel keeps:
    PID
    exit status = 42
       │
       │ waitpid()
       ▼
Parent receives 42
       │
       ▼
Kernel removes process entry
```

### Why is this especially relevant to shells?

For a foreground command:

```bash
command
```

the shell naturally does:

```c
waitpid(pid, ...);
```

before displaying the next prompt.

But for:

```bash
command &
```

the shell must **not block waiting for that process**, because that would defeat background execution.

Instead, a shell can periodically reap finished children using:

```c
waitpid(-1, &status, WNOHANG);
```

`WNOHANG` means:

> "Check whether a child has finished, but don't block if none has."

So a background-aware shell needs two separate behaviors:

```text
Foreground:
    fork()
      ↓
    exec()
      ↓
    waitpid()        ← block until finished

Background:
    fork()
      ↓
    exec()
      ↓
    continue shell   ← don't block
      ↓
    later reap with waitpid(..., WNOHANG)
```

**In short:** a child becomes a zombie because it has **exited but its parent hasn't yet collected its exit status**. `waitpid()` performs that collection and allows the kernel to fully remove the child's process entry.

## You

how to fix this

## ChatGPT

If you mean **how to prevent background processes in your C shell from becoming zombies**, the fix is to reap them with `waitpid()` **without blocking the shell**.

### Simplest fix: `waitpid(..., WNOHANG)`

After launching a background process, don't do:

```c
waitpid(pid, NULL, 0);
```

because that makes the shell wait.

Instead, periodically reap completed children:

```c
void reap_background_processes(void)
{
    int status;

    while (waitpid(-1, &status, WNOHANG) > 0) {
        // Child has been reaped.
    }
}
```

Then call this in your shell loop:

```c
while (1) {
    reap_background_processes();

    printf("$ ");
    fflush(stdout);

    // read and execute next command
}
```

### Why this works

Suppose:

```bash
sleep 5 &
echo hello
```

Your shell does:

```text
fork()
  │
  ├── child → sleep 5
  │
  └── parent → immediately continues
                    │
                    ▼
                  echo hello
```

When `sleep` finishes:

```text
sleep exits
    ↓
becomes zombie temporarily
    ↓
reap_background_processes()
    ↓
waitpid(-1, ..., WNOHANG)
    ↓
zombie removed
```

`WNOHANG` is important because it means the shell **doesn't stop and wait** if no child has finished.

---

### Better solution: `SIGCHLD`

For a more proper shell implementation, you can have the kernel notify your shell whenever a child terminates:

```c
void sigchld_handler(int sig)
{
    int status;

    while (waitpid(-1, &status, WNOHANG) > 0) {
        // Reap finished children
    }
}
```

Install it:

```c
signal(SIGCHLD, sigchld_handler);
```

Then:

```text
background child exits
        ↓
    SIGCHLD
        ↓
shell's handler
        ↓
waitpid(..., WNOHANG)
        ↓
child reaped
```

This lets your shell continue accepting commands while automatically cleaning up finished background processes.

**For a shell assignment, if you haven't implemented signals yet, `waitpid(-1, &status, WNOHANG)` in the main shell loop is usually the easiest approach.**

## You

How do I parse a complex command line string in C into sequentially executed commands (split by ⁠ ; ⁠) and background jobs (split by ⁠ & ⁠), preserving quoted strings and escaping

## ChatGPT

Don't use `strtok()` for this. It cannot reliably distinguish separators inside quotes or handle escaping.

For example, you want:

```text
echo "hello; world" ; sleep 2 & echo "a & b"
```

to become:

```text
Sequential command 1:
    echo "hello; world"

Background job:
    sleep 2

Sequential command 2:
    echo "a & b"
```

while:

```text
echo "hello; world"
```

must **not** be split at the `;` inside the quotes.

A good approach is a **single-pass parser with state**.

### Parser

:::writing{variant="document" id="74126" title="Parsing ; and & While Preserving Quotes and Escapes"}
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_COMMANDS 100
#define MAX_LEN      1024

typedef enum {
    NORMAL,
    SINGLE_QUOTE,
    DOUBLE_QUOTE
} ParseState;

/*
 * Parse a command line into separate commands.
 *
 * ; = execute previous command, then continue
 * & = execute previous command in background, then continue
 *
 * Quotes and escaped characters are preserved in the resulting
 * strings. Separators inside quotes are NOT treated as separators.
 */
int parse_commands(const char *line,
                   char *commands[],
                   int background[])
{
    char buffer[MAX_LEN];
    int buf_pos = 0;
    int command_count = 0;

    ParseState state = NORMAL;
    int escaped = 0;

    for (int i = 0; ; i++) {

        char c = line[i];

        /* End of input */
        if (c == '\0') {

            /* Add final command if there is one */
            if (buf_pos > 0) {
                buffer[buf_pos] = '\0';

                commands[command_count] = strdup(buffer);
                background[command_count] = 0;

                command_count++;
            }

            break;
        }

        /* Handle escaped character */
        if (escaped) {
            buffer[buf_pos++] = c;
            escaped = 0;
            continue;
        }

        /*
         * Backslash escapes the next character outside
         * or inside double quotes.
         */
        if (c == '\\' && state != SINGLE_QUOTE) {
            escaped = 1;

            /*
             * Keep the backslash if you want later parsing
             * to see it. Remove this line if you want the
             * parser to consume the escape here.
             */
            buffer[buf_pos++] = c;

            continue;
        }

        /* Handle single quotes */
        if (c == '\'' && state == NORMAL) {
            state = SINGLE_QUOTE;
            buffer[buf_pos++] = c;
            continue;
        }

        if (c == '\'' && state == SINGLE_QUOTE) {
            state = NORMAL;
            buffer[buf_pos++] = c;
            continue;
        }

        /* Handle double quotes */
        if (c == '"' && state == NORMAL) {
            state = DOUBLE_QUOTE;
            buffer[buf_pos++] = c;
            continue;
        }

        if (c == '"' && state == DOUBLE_QUOTE) {
            state = NORMAL;
            buffer[buf_pos++] = c;
            continue;
        }

        /*
         * Only recognize ; and & as separators when
         * we're outside quotes.
         */
        if (state == NORMAL && (c == ';' || c == '&')) {

            /* Remove trailing whitespace */
            while (buf_pos > 0 &&
                   isspace((unsigned char)buffer[buf_pos - 1])) {
                buf_pos--;
            }

            /*
             * Ignore empty commands.
             *
             * Depending on your shell specification, you might
             * instead want to report these as syntax errors.
             */
            if (buf_pos > 0) {
                buffer[buf_pos] = '\0';

                commands[command_count] = strdup(buffer);

                /*
                 * '&' means background.
                 * ';' means foreground/sequential.
                 */
                background[command_count] = (c == '&');

                command_count++;
            }

            buf_pos = 0;

            if (command_count >= MAX_COMMANDS) {
                fprintf(stderr, "too many commands\n");
                break;
            }

            continue;
        }

        /* Normal character */
        buffer[buf_pos++] = c;

        if (buf_pos >= MAX_LEN - 1) {
            fprintf(stderr, "command too long\n");
            break;
        }
    }

    /* Unterminated quote */
    if (state != NORMAL) {
        fprintf(stderr, "syntax error: unterminated quote\n");
        return -1;
    }

    /* Trailing backslash */
    if (escaped) {
        fprintf(stderr, "syntax error: trailing escape character\n");
        return -1;
    }

    return command_count;
}
```
:::

### Using it

```c id="hglj8j"
int main(void)
{
    char *commands[MAX_COMMANDS];
    int background[MAX_COMMANDS];

    const char *line =
        "echo \"hello; world\" ; sleep 2 & echo \"a & b\"";

    int n = parse_commands(line, commands, background);

    if (n < 0)
        return 1;

    for (int i = 0; i < n; i++) {
        printf("Command %d: [%s]\n", i + 1, commands[i]);
        printf("Background: %s\n",
               background[i] ? "yes" : "no");

        free(commands[i]);
    }

    return 0;
}
```

You'd get approximately:

```text
Command 1: [echo "hello; world"]
Background: no

Command 2: [sleep 2]
Background: yes

Command 3: [echo "a & b"]
Background: no
```

---

## The important idea: parser state

At any point, your parser is in one of three states:

```text
NORMAL
   │
   ├── '  → SINGLE_QUOTE
   └── "  → DOUBLE_QUOTE


SINGLE_QUOTE
   │
   └── ' → NORMAL


DOUBLE_QUOTE
   │
   └── " → NORMAL
```

Only when you're in `NORMAL` do these have special meaning:

```text
;  → sequential execution
&  → background execution
```

So:

```text
echo "hello;world"
           ↑
       DOUBLE_QUOTE
```

doesn't split.

Likewise:

```text
echo 'a & b'
       ↑
   SINGLE_QUOTE
```

doesn't split.

And an escaped separator:

```text
echo hello\;world
           ↑
       escaped
```

also doesn't split.

---

## One important architectural point

For a shell, I'd actually recommend **two parsing stages**:

```text
Raw command line
       │
       ▼
Stage 1: split on ; and &
       │
       ├── command 1
       ├── command 2
       └── command 3
       │
       ▼
Stage 2: parse each command
       │
       ├── arguments
       ├── <
       ├── >
       ├── |
       └── filenames
       │
       ▼
execute_pipeline()
```

For example:

```bash
cat input.txt | grep "a;b" > output.txt & echo done
```

Stage 1 should produce:

```text
Command 1:
cat input.txt | grep "a;b" > output.txt
background = true

Command 2:
echo done
background = false
```

Then your **pipeline parser** handles `|`, `<`, and `>` inside each individual command.

This separation makes the implementation much easier to reason about and avoids having one enormous parser trying to handle `;`, `&`, `|`, redirections, quotes, and escapes simultaneously.

## You

Write a C signal handler for ⁠ SIGCHLD ⁠

## ChatGPT

For a shell, the usual `SIGCHLD` handler should **reap all children that have already terminated without blocking**.

```c
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

void sigchld_handler(int sig)
{
    int saved_errno = errno;
    int status;

    while (waitpid(-1, &status, WNOHANG) > 0) {
        // Child has been reaped.
    }

    errno = saved_errno;
}
```

You also need:

```c
#include <errno.h>
```

### Install the handler

Prefer `sigaction()` over `signal()`:

```c
struct sigaction sa;

sa.sa_handler = sigchld_handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

sigaction(SIGCHLD, &sa, NULL);
```

### Why the `while` loop?

Multiple children can finish before your shell handles `SIGCHLD`. You may receive only one notification even though several children have exited.

So instead of:

```c
waitpid(-1, &status, WNOHANG);
```

use:

```c
while (waitpid(-1, &status, WNOHANG) > 0)
    ;
```

This keeps reaping children until there are no more terminated children.

### Why `WNOHANG`?

Without it:

```c
waitpid(-1, &status, 0);
```

could block the shell if no child has exited.

With:

```c
waitpid(-1, &status, WNOHANG);
```

the handler immediately returns if there is nothing to reap.

So the flow for a background process is:

```text
shell
  │
  ├── fork()
  │     │
  │     └── child → execvp()
  │
  └── continues accepting commands
              │
              │ child exits
              ▼
          SIGCHLD
              │
              ▼
      sigchld_handler()
              │
              ▼
      waitpid(..., WNOHANG)
              │
              ▼
          child reaped
```

One caveat: **don't use `printf()`, `malloc()`, or other non-async-signal-safe functions inside the handler.** If you want to print `"Process finished"` or similar, record the event and handle the printing in the main shell loop.

## You

how to implement an ⁠ activities ⁠ command that lists all running/stopped background processes. explain in detail

## ChatGPT

For an `activities` command in your shell, the main idea is:

> **Your shell must maintain its own list of background jobs.**

You cannot simply call `waitpid()` when the user types `activities` and expect it to tell you every background process. The shell needs to record each background job when it is created, update its state when it stops/continues/exits, and print that stored information when `activities` is entered.

Suppose your shell supports:

```bash
sleep 100 &
vim &
```

and then:

```bash
activities
```

You might want:

```text
[12345] : sleep 100 - Running
[12346] : vim - Stopped
```

## 1. Create a job structure

First, maintain information about each background process:

```c
#define MAX_JOBS 100
#define CMD_LEN 1024

typedef enum {
    RUNNING,
    STOPPED
} JobState;

typedef struct {
    pid_t pid;
    char command[CMD_LEN];
    JobState state;
    int active;
} Job;

Job jobs[MAX_JOBS];
```

For example, after:

```bash
sleep 100 &
```

your array might contain:

```text
jobs[0]
    pid     = 12345
    command = "sleep 100"
    state   = RUNNING
    active  = 1
```

`active` tells you whether that slot currently represents a background job.

---

## 2. Add a job when you create a background process

Suppose your normal execution code looks roughly like:

```c
pid_t pid = fork();

if (pid == 0) {
    execvp(args[0], args);
    perror("execvp");
    exit(1);
}

if (background) {
    // Don't wait
} else {
    waitpid(pid, NULL, 0);
}
```

For a background command, add it to the job table:

```c
void add_job(pid_t pid, const char *command)
{
    for (int i = 0; i < MAX_JOBS; i++) {

        if (!jobs[i].active) {
            jobs[i].pid = pid;
            jobs[i].state = RUNNING;
            jobs[i].active = 1;

            strncpy(jobs[i].command,
                    command,
                    CMD_LEN - 1);

            jobs[i].command[CMD_LEN - 1] = '\0';

            return;
        }
    }

    fprintf(stderr, "Too many background jobs\n");
}
```

Then:

```c
if (background) {
    add_job(pid, original_command);
} else {
    waitpid(pid, NULL, 0);
}
```

Now the shell remembers the process even though it isn't waiting for it.

---

# 3. How do we know whether a process is Running or Stopped?

This is where `waitpid()` becomes important.

A child can undergo several state changes:

```text
                  SIGSTOP / Ctrl-Z
RUNNING ─────────────────────────────→ STOPPED
   ▲                                     │
   │                                     │
   └────────── SIGCONT ──────────────────┘


RUNNING ───────── exit ─────────→ TERMINATED
```

Your shell needs to notice these changes.

Use:

```c
waitpid(pid, &status,
        WNOHANG | WUNTRACED | WCONTINUED);
```

The flags mean:

```text
WNOHANG
    Don't block if nothing happened.

WUNTRACED
    Tell me if a child was stopped.

WCONTINUED
    Tell me if a stopped child resumed.
```

Then inspect `status`:

```c
if (WIFEXITED(status)) {
    // Process exited normally
}

else if (WIFSIGNALED(status)) {
    // Process terminated because of a signal
}

else if (WIFSTOPPED(status)) {
    // Process stopped
}

else if (WIFCONTINUED(status)) {
    // Process resumed
}
```

---

# 4. Update your job table

You can write:

```c
void update_jobs(void)
{
    int status;

    for (int i = 0; i < MAX_JOBS; i++) {

        if (!jobs[i].active)
            continue;

        pid_t result = waitpid(
            jobs[i].pid,
            &status,
            WNOHANG | WUNTRACED | WCONTINUED
        );

        if (result == 0) {
            /*
             * No state change.
             * Keep existing state.
             */
            continue;
        }

        if (result == -1) {
            continue;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {

            /* Job has finished */
            jobs[i].active = 0;

        } else if (WIFSTOPPED(status)) {

            jobs[i].state = STOPPED;

        } else if (WIFCONTINUED(status)) {

            jobs[i].state = RUNNING;
        }
    }
}
```

Now your shell's internal job table stays reasonably current.

---

# 5. Implement `activities`

Once the table exists, the command itself is simple:

```c
void print_activities(void)
{
    update_jobs();

    for (int i = 0; i < MAX_JOBS; i++) {

        if (!jobs[i].active)
            continue;

        printf("[%d] : %s - %s\n",
               jobs[i].pid,
               jobs[i].command,
               jobs[i].state == RUNNING
                   ? "Running"
                   : "Stopped");
    }
}
```

Then when parsing built-in commands:

```c
if (strcmp(args[0], "activities") == 0) {
    print_activities();
    continue;
}
```

So:

```bash
sleep 100 &
sleep 200 &
activities
```

could produce:

```text
[15423] : sleep 100 - Running
[15424] : sleep 200 - Running
```

---

# 6. But what about your `SIGCHLD` handler?

This is where your previous `SIGCHLD` implementation needs some thought.

If you have:

```c
void sigchld_handler(int sig)
{
    int status;

    while (waitpid(-1, &status, WNOHANG) > 0)
        ;
}
```

you're **reaping children but throwing away all the information about which child finished**.

For a shell with job tracking, that's inconvenient.

You need the shell to know:

```text
PID 1234 → exited
PID 1235 → stopped
PID 1236 → continued
```

One simple design for an assignment shell is to have your main shell code call `update_jobs()` regularly rather than doing all job bookkeeping inside the signal handler.

For example:

```c
while (1) {

    update_jobs();

    printf("> ");
    fflush(stdout);

    /* read command */

    ...
}
```

Because `update_jobs()` uses:

```c
WNOHANG
```

it won't block your shell.

---

# 7. A cleaner `SIGCHLD` design

Another approach is for the signal handler to simply notify the main loop that something changed.

Use a global flag:

```c
volatile sig_atomic_t child_changed = 0;
```

Handler:

```c
void sigchld_handler(int sig)
{
    child_changed = 1;
}
```

Install it:

```c
struct sigaction sa;

sa.sa_handler = sigchld_handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART;

sigaction(SIGCHLD, &sa, NULL);
```

Then in your main loop:

```c
while (1) {

    if (child_changed) {
        child_changed = 0;
        update_jobs();
    }

    printf("> ");
    fflush(stdout);

    ...
}
```

This has a nice separation:

```text
SIGCHLD handler
      │
      │ "something happened"
      ▼
child_changed = 1
      │
      ▼
Main shell
      │
      ▼
update_jobs()
      │
      ├── exited?    → remove job
      ├── stopped?   → mark STOPPED
      └── continued? → mark RUNNING
```

The signal handler stays tiny and avoids doing complex work asynchronously.

---

# 8. There's an important race condition

Suppose you do:

```c
pid = fork();
```

The child could theoretically:

```text
fork
 │
 ├── child executes extremely quickly
 │
 ├── child exits
 │
 └── SIGCHLD delivered
```

before the parent has done:

```c
add_job(pid, command);
```

For a more robust implementation, block `SIGCHLD` around `fork()` and adding the job.

For example:

```c
sigset_t mask, oldmask;

sigemptyset(&mask);
sigaddset(&mask, SIGCHLD);

sigprocmask(SIG_BLOCK, &mask, &oldmask);

pid_t pid = fork();

if (pid == 0) {

    /* Child restores signal mask */
    sigprocmask(SIG_SETMASK, &oldmask, NULL);

    execvp(args[0], args);

    perror("execvp");
    _exit(1);
}

/* Parent */

if (background) {
    add_job(pid, original_command);
}

/* Now SIGCHLD can safely be delivered */
sigprocmask(SIG_SETMASK, &oldmask, NULL);
```

This ensures:

```text
block SIGCHLD
      ↓
    fork()
      ↓
add child to jobs[]
      ↓
unblock SIGCHLD
```

rather than potentially:

```text
fork()
  ↓
child exits
  ↓
SIGCHLD
  ↓
??? child isn't in jobs[] yet
  ↓
add_job()
```

---

# 9. What about stopped foreground processes?

This is another important case.

Suppose the user runs:

```bash
sleep 100
```

It's initially a foreground process, so it isn't in your background job list.

But then the user presses Ctrl-Z.

The foreground process becomes:

```text
RUNNING → STOPPED
```

Your foreground `waitpid()` should therefore use:

```c
waitpid(pid, &status, WUNTRACED);
```

Then:

```c
if (WIFSTOPPED(status)) {
    add_job(pid, original_command);

    // Change newly-added job to STOPPED
}
```

For example:

```c
int status;

waitpid(pid, &status, WUNTRACED);

if (WIFSTOPPED(status)) {

    add_job(pid, original_command);

    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active &&
            jobs[i].pid == pid) {

            jobs[i].state = STOPPED;
            break;
        }
    }
}
```

Now:

```bash
sleep 100
^Z
activities
```

can correctly show:

```text
[12345] : sleep 100 - Stopped
```

---

# 10. Putting the architecture together

Your shell can be structured roughly like this:

```text
                    SHELL
                      │
                read command
                      │
                      ▼
                   parse
                      │
            ┌─────────┴──────────┐
            │                    │
         built-in             external
            │                    │
            │                  fork()
            │                    │
            │            ┌───────┴────────┐
            │            │                │
            │          child            parent
            │            │                │
            │         execvp()       background?
            │                             │
            │                    ┌────────┴────────┐
            │                   YES               NO
            │                    │                 │
            │                 add_job()        waitpid()
            │                                      │
            │                                   stopped?
            │                                      │
            │                                    YES
            │                                      │
            │                                   add_job()
            │
            ▼
      activities command
            │
            ▼
       update_jobs()
            │
            ▼
       print jobs[]
```

## A minimal combined implementation

The core pieces can look like:

```c
typedef enum {
    RUNNING,
    STOPPED
} JobState;

typedef struct {
    pid_t pid;
    char command[1024];
    JobState state;
    int active;
} Job;

#define MAX_JOBS 100

Job jobs[MAX_JOBS];
volatile sig_atomic_t child_changed = 0;


void sigchld_handler(int sig)
{
    child_changed = 1;
}


void add_job(pid_t pid, const char *command)
{
    for (int i = 0; i < MAX_JOBS; i++) {

        if (!jobs[i].active) {

            jobs[i].pid = pid;
            jobs[i].state = RUNNING;
            jobs[i].active = 1;

            strncpy(jobs[i].command,
                    command,
                    sizeof(jobs[i].command) - 1);

            jobs[i].command[
                sizeof(jobs[i].command) - 1
            ] = '\0';

            return;
        }
    }
}


void update_jobs(void)
{
    int status;

    for (int i = 0; i < MAX_JOBS; i++) {

        if (!jobs[i].active)
            continue;

        pid_t result = waitpid(
            jobs[i].pid,
            &status,
            WNOHANG | WUNTRACED | WCONTINUED
        );

        if (result <= 0)
            continue;

        if (WIFEXITED(status) ||
            WIFSIGNALED(status)) {

            jobs[i].active = 0;

        } else if (WIFSTOPPED(status)) {

            jobs[i].state = STOPPED;

        } else if (WIFCONTINUED(status)) {

            jobs[i].state = RUNNING;
        }
    }
}


void activities(void)
{
    update_jobs();

    for (int i = 0; i < MAX_JOBS; i++) {

        if (!jobs[i].active)
            continue;

        printf("[%d] : %s - %s\n",
               jobs[i].pid,
               jobs[i].command,
               jobs[i].state == RUNNING
                    ? "Running"
                    : "Stopped");
    }
}
```

The most important conceptual point is that **`activities` isn't responsible for discovering jobs from scratch**. Your shell builds and maintains the job table throughout its lifetime:

```text
background process created
        ↓
     add_job()
        ↓
   ┌─────────────┐
   │  JOB TABLE  │
   │             │
   │ PID         │
   │ command     │
   │ state       │
   └─────────────┘
        ↑
        │
   update_jobs()
        ↑
        │
      SIGCHLD

activities
    ↓
read job table
    ↓
print Running/Stopped jobs
```

One further complication arises if your background job is a **pipeline**, such as `cat file | grep x &`: then one "job" contains multiple child PIDs. A more complete shell should track a **process group ID (PGID)** plus the PIDs in that pipeline, rather than treating every PID as a separate activity.

## You

Explain process groups, foreground process group nd controlling terminals in linux in detail to help me code c shell

## ChatGPT

If you're implementing a C shell with **foreground/background execution, Ctrl-C, Ctrl-Z, `jobs`/`activities`, and pipelines**, process groups and controlling terminals are the part that makes everything fit together.

The key idea is:

> A shell doesn't really manage foreground/background status one process at a time. It manages **process groups** and decides which process group currently owns the terminal.

---

# 1. Why process groups are needed

Consider:

```bash
cat file.txt | grep hello | wc -l
```

Your shell creates **three processes**:

```text
shell
 ├── cat
 ├── grep
 └── wc
```

But from the user's perspective, this is **one job**.

If the user presses:

```text
Ctrl-C
```

you don't want only `cat` to die.

You want:

```text
cat    ← SIGINT
grep   ← SIGINT
wc     ← SIGINT
```

to all receive the signal.

That's why Unix has **process groups**.

Instead of treating:

```text
cat
grep
wc
```

as three unrelated processes, the shell puts them into one process group:

```text
Process Group
PGID = 5000

    ├── cat
    ├── grep
    └── wc
```

Then a signal can be sent to the **entire group**.

---

# 2. Process IDs vs process group IDs

Every process has:

```text
PID  = Process ID
PGID = Process Group ID
```

For example:

```text
PID       PGID
1000      1000     shell
1001      1001     cat
1002      1001     grep
1003      1001     wc
```

Here:

```text
cat, grep, wc
```

are all in process group:

```text
PGID = 1001
```

Usually, the first process created for a job becomes the **process-group leader**, and its PID becomes the PGID:

```text
cat:
PID  = 1001
PGID = 1001
```

Then:

```text
grep:
PID  = 1002
PGID = 1001

wc:
PID  = 1003
PGID = 1001
```

You can get the current process's group with:

```c
getpgrp();
```

and change a process's group with:

```c
setpgid(pid, pgid);
```

---

# 3. Creating a process group

Suppose your shell does:

```c
pid_t pid = fork();
```

In the child, you can create a new process group:

```c
setpgid(0, 0);
```

The arguments mean:

```text
setpgid(0, 0)
         │  │
         │  └── use my PID as PGID
         └───── change myself
```

So if the child has:

```text
PID = 1234
```

after:

```c
setpgid(0, 0);
```

you get:

```text
PID  = 1234
PGID = 1234
```

That child is now the process-group leader.

For a pipeline, subsequent children should join that same group:

```c
setpgid(0, pgid);
```

where `pgid` is the first child's PID.

So:

```text
             PGID 1234
                 │
       ┌─────────┼─────────┐
       │         │         │
     cat       grep        wc
    PID1234   PID1235    PID1236
```

---

# 4. What is the controlling terminal?

When you're running an interactive shell in a terminal:

```text
Terminal emulator
       │
       ▼
   pseudo-terminal
       │
       ▼
      shell
```

The shell has a **controlling terminal**.

You can generally interact with it through:

```c
STDIN_FILENO
STDOUT_FILENO
STDERR_FILENO
```

and the terminal device has special job-control behavior.

The important thing is:

> The terminal keeps track of **which process group is currently in the foreground**.

You can think of it as:

```text
Terminal
   │
   └── foreground PGID = 1234
```

If:

```text
PGID 1234 = cat | grep | wc
```

then that entire pipeline is the foreground job.

---

# 5. Foreground process group

Suppose your shell is:

```text
PGID = 1000
```

and you run:

```bash
sleep 100
```

The shell creates:

```text
shell
PGID 1000

sleep
PGID 2000
```

The shell then gives the terminal to process group 2000:

```c
tcsetpgrp(STDIN_FILENO, 2000);
```

Now:

```text
Terminal
    │
    ▼
foreground PGID = 2000
                         │
                         ▼
                    sleep process
```

The shell is still alive, but it is no longer the foreground process group.

---

# 6. Why does this matter for Ctrl-C?

This is the most important part.

When you press:

```text
Ctrl-C
```

the terminal driver generates:

```text
SIGINT
```

and sends it to the **foreground process group**.

It doesn't simply send it to whichever process called `read()`.

So if:

```text
Terminal
   │
   └── foreground PGID = 2000

PGID 2000:
   ├── cat
   ├── grep
   └── wc
```

pressing Ctrl-C causes:

```text
SIGINT → cat
SIGINT → grep
SIGINT → wc
```

The shell doesn't have to manually detect Ctrl-C and kill every child.

The terminal's job-control mechanism handles it.

---

# 7. Ctrl-Z works the same way

When the user presses:

```text
Ctrl-Z
```

the terminal generates:

```text
SIGTSTP
```

and sends it to the foreground process group.

So:

```text
Ctrl-Z
    ↓
SIGTSTP
    ↓
foreground PGID
    ↓
┌───────┬───────┬───────┐
cat    grep     wc
```

They can all become stopped.

The shell can then detect this using:

```c
waitpid(..., &status, WUNTRACED);
```

and:

```c
WIFSTOPPED(status)
```

---

# 8. Foreground vs background

This is where process groups and terminal ownership come together.

Suppose:

```bash
sleep 100 &
```

The shell creates a process group:

```text
PGID 2000
    │
  sleep
```

but **doesn't give the terminal to it**.

The terminal remains:

```text
foreground PGID = shell's PGID
```

So:

```text
Terminal
   │
   └── foreground → Shell

Background:
   PGID 2000
       └── sleep
```

The shell can immediately display:

```text
$
```

and accept another command.

---

# 9. Why can't a background process just read from the terminal?

Suppose:

```bash
cat &
```

`cat` tries to read from stdin.

But `cat` is in a **background process group**.

The terminal can send:

```text
SIGTTIN
```

to a background process that attempts to read from the controlling terminal.

So the process may be stopped.

Similarly, certain terminal operations from a background process can generate:

```text
SIGTTOU
```

This is another reason the shell needs to correctly manage terminal ownership.

---

# 10. `tcsetpgrp()` is the crucial function

This function changes which process group owns the terminal's foreground status:

```c
tcsetpgrp(STDIN_FILENO, pgid);
```

Suppose your shell has:

```text
Shell PGID = 1000
Job PGID   = 2000
```

To run the job in the foreground:

```c
tcsetpgrp(STDIN_FILENO, 2000);
```

Now:

```text
Terminal
    │
    └── foreground PGID = 2000
```

After the job finishes or stops, the shell must take the terminal back:

```c
tcsetpgrp(STDIN_FILENO, 1000);
```

So the sequence is:

```text
             shell
               │
             fork
               │
               ▼
          create job
               │
               ▼
       set process group
               │
               ▼
    tcsetpgrp(terminal, job_pgid)
               │
               ▼
          run foreground
               │
          ┌────┴────┐
          │         │
        exits     stops
          │         │
          └────┬────┘
               ▼
    tcsetpgrp(terminal, shell_pgid)
               │
               ▼
          shell continues
```

---

# 11. The shell itself should have a process group

An interactive shell normally puts itself in its own process group.

Conceptually:

```text
Shell
PID  = 1000
PGID = 1000
```

Then:

```text
Job 1
PGID = 2000

Job 2
PGID = 3000
```

The terminal switches between them:

```text
Terminal
   │
   ├── shell PGID 1000
   │
   ├── job 1 PGID 2000
   │
   └── job 2 PGID 3000
```

Only one can be the foreground process group at a time.

---

# 12. How this works with a pipeline

This is particularly important for your shell.

Suppose the user enters:

```bash
cat file.txt | grep hello | wc -l
```

You create three children:

```text
cat    PID 2001
grep   PID 2002
wc     PID 2003
```

Put them all in one process group:

```text
PGID = 2001

2001 cat
2002 grep
2003 wc
```

Then:

```c
tcsetpgrp(STDIN_FILENO, 2001);
```

Now the terminal considers the **entire pipeline** the foreground job.

If Ctrl-C happens:

```text
SIGINT
  │
  ▼
PGID 2001
  ├── cat
  ├── grep
  └── wc
```

All three are affected.

This is why you should think:

> **One pipeline = one job = one process group.**

---

# 13. `kill()` can also target the whole group

You can explicitly send a signal to a process group using a negative PID:

```c
kill(-pgid, SIGINT);
```

For example:

```c
kill(-2001, SIGTERM);
```

means:

> Send SIGTERM to every process in process group 2001.

This is useful for shell built-ins such as:

```text
kill
```

or job-control operations.

---

# 14. `waitpid()` and process groups

You can also wait for processes belonging to a particular process group.

For example:

```c
waitpid(-pgid, &status, WUNTRACED);
```

A negative first argument means:

> Wait for any child whose process group ID is `pgid`.

For a pipeline:

```text
PGID 2001
 ├── cat
 ├── grep
 └── wc
```

you can use:

```c
waitpid(-2001, &status, WUNTRACED);
```

to wait for a child in that process group.

For a shell, though, you usually need to keep track of **all processes in the job** and their states rather than assuming one `waitpid()` tells you everything about the pipeline.

---

# 15. Foreground job execution

A simplified foreground pipeline algorithm looks like:

```c
pgid = 0;

for (each command) {

    fork();

    if (child) {

        if (pgid == 0)
            pgid = getpid();

        setpgid(0, pgid);

        // dup2() pipe/file descriptors

        execvp(...);
    }

    // parent
    if (pgid == 0)
        pgid = pid;

    setpgid(pid, pgid);
}

/* Give terminal to pipeline */
tcsetpgrp(STDIN_FILENO, pgid);

/* Wait for pipeline */
wait_for_job(pgid);

/* Take terminal back */
tcsetpgrp(STDIN_FILENO, shell_pgid);
```

Notice that **both parent and child call `setpgid()`** in robust implementations.

That's intentional because there's a race between parent and child execution.

---

# 16. Why both parent and child call `setpgid()`?

Suppose:

```text
Parent                         Child
   │                              │
 fork()                           │
   │                              │
   │                         execvp()
   │                              │
setpgid(child, pgid)              │
```

The child could call `execvp()` before the parent manages to move it into the desired process group.

Therefore shells commonly have both sides participate:

### Child

```c
setpgid(0, pgid);
```

### Parent

```c
setpgid(pid, pgid);
```

This makes the setup more robust against scheduling races.

---

# 17. Background job execution

For:

```bash
cat file | grep hello &
```

you still create one process group:

```text
PGID 2000
 ├── cat
 └── grep
```

But you **do not** call:

```c
tcsetpgrp(STDIN_FILENO, 2000);
```

The shell keeps the terminal:

```text
Terminal
   │
   └── foreground PGID = shell
```

while the pipeline runs in the background.

You also don't perform a blocking foreground wait.

Instead:

```text
create pipeline
      ↓
create process group
      ↓
add job to job table
      ↓
return immediately to prompt
```

---

# 18. Bringing a background job to the foreground

This is what a `fg` command eventually needs to do.

Suppose:

```text
jobs:
[1] PGID 2000  sleep 100 - Stopped
```

User runs:

```bash
fg 1
```

The shell should roughly:

### Step 1 — give terminal to job

```c
tcsetpgrp(STDIN_FILENO, 2000);
```

### Step 2 — continue the stopped group

```c
kill(-2000, SIGCONT);
```

### Step 3 — wait

```c
waitpid(...);
```

### Step 4 — take terminal back

```c
tcsetpgrp(STDIN_FILENO, shell_pgid);
```

Conceptually:

```text
Stopped job
    │
    │ fg
    ▼
SIGCONT
    │
    ▼
terminal → job PGID
    │
    ▼
job runs
    │
    ├── exits
    │
    └── stops again
    │
    ▼
terminal → shell PGID
```

---

# 19. How `activities` fits into this

Your `activities` command can now be based on **jobs/process groups**, rather than just individual PIDs.

A useful job structure is:

```c
typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} JobState;

typedef struct {
    int job_id;
    pid_t pgid;

    char command[1024];

    JobState state;

    int num_processes;
    pid_t pids[100];
} Job;
```

For:

```bash
cat file | grep hello | wc -l &
```

you might have:

```text
Job ID: 1
PGID: 5000

Processes:
    5000 cat
    5001 grep
    5002 wc

State:
    RUNNING
```

Then:

```text
activities
```

can print one entry for the **whole job**, rather than three unrelated entries.

---

# 20. Process state vs job state

This distinction is important.

Imagine:

```text
cat | grep | wc
```

and suppose:

```text
cat     RUNNING
grep    STOPPED
wc      STOPPED
```

What should the job be called?

Your shell needs a policy for determining the overall job state.

A common conceptual model is:

```text
If the whole job is stopped → STOPPED

If at least one process is still running → RUNNING

If all processes have exited → DONE
```

For a simple assignment, you may be able to simplify this depending on the required specification.

---

# 21. Controlling terminal vs stdin

This distinction is often confusing.

A process having:

```text
stdin = terminal
```

doesn't by itself mean it is the foreground process.

The terminal additionally has a **foreground process group ID**.

Think of:

```text
FD 0
 │
 ▼
terminal device
 │
 └── foreground PGID = 2000
```

If your process is in PGID 2000:

```text
foreground → yes
```

If your process is in PGID 3000:

```text
background → yes
```

even though both might have FD 0 referring to the same terminal.

---

# 22. Useful functions to know

For your shell, these are the important APIs:

### Process groups

```c
getpid();
getpgrp();
setpgid(pid, pgid);
```

### Terminal

```c
tcgetpgrp(fd);
tcsetpgrp(fd, pgid);
```

### Signals

```c
kill(-pgid, SIGINT);
kill(-pgid, SIGTSTP);
kill(-pgid, SIGCONT);
```

### Waiting

```c
waitpid(pid, &status, ...);
waitpid(-pgid, &status, ...);
```

### Status checking

```c
WIFEXITED(status)
WIFSIGNALED(status)
WIFSTOPPED(status)
WIFCONTINUED(status)
```

---

# 23. The overall shell architecture

If you're implementing all the features you've been asking about, your shell is starting to look like this:

```text
                         SHELL
                           │
                           │ parse
                           ▼
                  ┌─────────────────┐
                  │ command/job     │
                  │ identification  │
                  └────────┬────────┘
                           │
                           ▼
                    create pipeline
                           │
                    n children
                           │
                           ▼
                  ┌─────────────────┐
                  │  Process Group  │
                  │      PGID       │
                  ├─────────────────┤
                  │ cmd1            │
                  │ cmd2            │
                  │ cmd3            │
                  └────────┬────────┘
                           │
                 ┌─────────┴─────────┐
                 │                   │
             foreground          background
                 │                   │
                 ▼                   ▼
        tcsetpgrp(terminal,    keep shell as
                   pgid)       foreground
                 │                   │
                 ▼                   ▼
              waitpid()          don't wait
                 │                   │
           ┌─────┴─────┐            │
           │           │            │
         exit        Ctrl-Z         │
           │           │            │
           │        stopped         │
           │           │            │
           └─────┬─────┘            │
                 ▼                  ▼
        terminal → shell       job table
                 │                  │
                 ▼                  │
              prompt ◄──────────────┘
```

### The four concepts to keep separate

| Concept | Meaning |
|---|---|
| **PID** | Identifies one process |
| **PGID** | Identifies a group of processes belonging to one job |
| **Controlling terminal** | Terminal associated with the interactive shell/session |
| **Foreground PGID** | Process group currently allowed to interact with that terminal |

If you remember only one relationship, make it this:

```text
                  CONTROLLING TERMINAL
                         │
                         │ has one
                         ▼
                  FOREGROUND PGID
                         │
                         │ identifies
                         ▼
                   PROCESS GROUP
                    /     |     \
                  cmd1   cmd2   cmd3
```

And for shell job control:

```text
                 one pipeline
                      │
                      ▼
                one process group
                      │
              ┌───────┴────────┐
              │                │
          foreground        background
              │                │
       terminal belongs    shell keeps
          to job            terminal
              │
          Ctrl-C/Z
              │
          signal goes
          to whole group
```

That is the core model you need to implement `&`, `Ctrl-C`, `Ctrl-Z`, `activities/jobs`, and eventually `fg`/`bg` correctly.

## You

How can I properly configure terminal signal masks in my C shell's ⁠ main() ⁠ function so that pressing ⁠ Ctrl+C ⁠ or ⁠ Ctrl+Z ⁠ doesn't crash the shell prompt only stops and kills, give code example

## ChatGPT

Yes. For an interactive shell, the important distinction is:

- **The shell should ignore `SIGINT` (`Ctrl+C`) and `SIGTSTP` (`Ctrl+Z`).**
- **Children should restore the default behavior** before `execvp()`.
- The foreground **process group** should receive terminal-generated signals, so `Ctrl+C`/`Ctrl+Z` affect the running command rather than the shell.
- Use `sigprocmask()` to ensure signals aren't accidentally blocked; signal **disposition** (`SIG_IGN`/`SIG_DFL`) is separate from the signal mask.

A good `main()` setup looks like this.

### 1. Shell initialization

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

static pid_t shell_pgid;
static int shell_terminal;

int main(void)
{
    shell_terminal = STDIN_FILENO;

    /*
     * Put the shell into its own process group.
     */
    shell_pgid = getpid();

    if (setpgid(shell_pgid, shell_pgid) < 0) {
        if (errno != EACCES && errno != EPERM) {
            perror("setpgid");
            exit(1);
        }
    }

    /*
     * Make the shell's process group the foreground
     * process group of the terminal.
     */
    if (tcsetpgrp(shell_terminal, shell_pgid) < 0) {
        perror("tcsetpgrp");
        exit(1);
    }

    /*
     * The shell itself should NOT be killed/stopped by
     * Ctrl+C / Ctrl+Z.
     */
    signal(SIGINT,  SIG_IGN);   // Ctrl+C
    signal(SIGTSTP, SIG_IGN);   // Ctrl+Z
    signal(SIGQUIT, SIG_IGN);

    /*
     * Prevent the shell from being stopped when it manipulates
     * the terminal while it is not the foreground group.
     */
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    /*
     * Main shell loop.
     */
    while (1) {
        printf("shell$ ");
        fflush(stdout);

        /*
         * Read and execute commands here.
         */
    }

    return 0;
}
```

However, for a real shell, I'd use `sigaction()` rather than `signal()`.

---

# 2. Recommended `sigaction()` version

Create a helper:

```c
void setup_shell_signals(void)
{
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = SIG_IGN;

    sigaction(SIGINT,  &sa, NULL);   // Ctrl+C
    sigaction(SIGTSTP, &sa, NULL);   // Ctrl+Z
    sigaction(SIGQUIT, &sa, NULL);

    /*
     * Important for terminal job control.
     */
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);
}
```

Then:

```c
int main(void)
{
    shell_terminal = STDIN_FILENO;
    shell_pgid = getpid();

    setpgid(shell_pgid, shell_pgid);

    tcsetpgrp(shell_terminal, shell_pgid);

    setup_shell_signals();

    while (1) {
        printf("shell$ ");
        fflush(stdout);

        /*
         * parse command
         * fork children
         * execute pipeline
         */
    }

    return 0;
}
```

---

# 3. But don't let the children inherit `SIG_IGN`

This is extremely important.

Suppose your shell does:

```c
signal(SIGINT, SIG_IGN);
```

and then:

```c
fork();
```

The child inherits the shell's signal disposition.

So without fixing it, you could get:

```text
Shell
 SIGINT = SIG_IGN
     │
     │ fork()
     ▼
Child
 SIGINT = SIG_IGN
```

Then `Ctrl+C` won't kill your command either.

The child must restore the default signal handling **before `execvp()`**:

```c
void reset_child_signals(void)
{
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);
}
```

Then your child does:

```c
pid_t pid = fork();

if (pid == 0) {

    /*
     * Child gets normal Ctrl+C / Ctrl+Z behavior.
     */
    reset_child_signals();

    /*
     * Put child into its job's process group.
     */
    setpgid(0, pgid);

    /*
     * Set up pipes/redirection.
     */
    dup2(...);

    execvp(args[0], args);

    perror("execvp");
    _exit(127);
}
```

---

# 4. Why process groups matter here

Suppose you run:

```bash
cat file.txt | grep hello
```

Your shell creates:

```text
Shell
PGID = 1000

Pipeline
PGID = 2000
 ├── cat
 └── grep
```

The shell should give the terminal to PGID 2000:

```c
tcsetpgrp(STDIN_FILENO, 2000);
```

Now the terminal looks conceptually like:

```text
             Terminal
                 │
                 ▼
        foreground PGID = 2000
                 │
             ┌───┴───┐
             │       │
            cat     grep
```

When you press:

```text
Ctrl+C
```

the terminal generates `SIGINT` for the **foreground process group**.

Therefore:

```text
Ctrl+C
   │
   ▼
 SIGINT
   │
   ▼
PGID 2000
 ├── cat   → killed
 └── grep  → killed
```

The shell is PGID 1000, so it doesn't receive that terminal-generated `SIGINT`.

---

# 5. After the command finishes, give terminal back to shell

Your foreground execution code should roughly be:

```c
/* Parent */

setpgid(pid, pgid);

/*
 * Give terminal to the foreground job.
 */
tcsetpgrp(STDIN_FILENO, pgid);

/*
 * Wait for the foreground job.
 */
waitpid(-pgid, &status, WUNTRACED);

/*
 * Take terminal back.
 */
tcsetpgrp(STDIN_FILENO, shell_pgid);
```

The overall sequence is:

```text
Shell PGID = 1000
       │
       │ fork
       ▼
Job PGID = 2000
       │
       │ tcsetpgrp()
       ▼
Terminal → PGID 2000
       │
       │
       ├──── Ctrl+C → SIGINT → job
       │
       └──── Ctrl+Z → SIGTSTP → job
                         │
                         ▼
                    job stops/exits
                         │
                         ▼
              tcsetpgrp(..., 1000)
                         │
                         ▼
                   Shell resumes
```

---

# 6. `Ctrl+C` vs `Ctrl+Z`

You don't normally need to manually detect the keyboard characters `Ctrl+C` or `Ctrl+Z`.

The terminal driver handles them.

### Ctrl+C

Generates:

```text
SIGINT
```

Normally:

```text
SIGINT → terminate process
```

### Ctrl+Z

Generates:

```text
SIGTSTP
```

Normally:

```text
SIGTSTP → stop/suspend process
```

Your shell ignores both:

```c
SIGINT  → SIG_IGN
SIGTSTP → SIG_IGN
```

while children use:

```c
SIGINT  → SIG_DFL
SIGTSTP → SIG_DFL
```

---

# 7. Where does the signal mask come in?

This is a slightly different concept from ignoring signals.

A **signal disposition** says:

```text
What should happen when SIGINT arrives?
```

For example:

```c
SIG_IGN
SIG_DFL
handler
```

A **signal mask** says:

```text
Should delivery of SIGINT be temporarily blocked?
```

You can explicitly unblock the terminal signals in the shell:

```c
sigset_t mask;

sigemptyset(&mask);
sigaddset(&mask, SIGINT);
sigaddset(&mask, SIGTSTP);
sigaddset(&mask, SIGQUIT);

sigprocmask(SIG_UNBLOCK, &mask, NULL);
```

And in the child:

```c
sigprocmask(SIG_UNBLOCK, &mask, NULL);
```

This is useful if your shell blocks signals elsewhere, for example while modifying its job table.

A good child setup is therefore:

```c
void reset_child_signals(void)
{
    struct sigaction sa;
    sigset_t mask;

    /*
     * Restore default dispositions.
     */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);

    /*
     * Unblock them too.
     */
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTSTP);
    sigaddset(&mask, SIGQUIT);

    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}
```

---

# 8. A very common mistake

Don't do this:

```c
signal(SIGINT, SIG_IGN);

pid = fork();

if (pid == 0) {
    execvp(...);
}
```

and assume the child will automatically get normal `Ctrl+C` behavior.

The child inherits the ignored disposition.

Instead:

```c
signal(SIGINT, SIG_IGN);

pid = fork();

if (pid == 0) {
    signal(SIGINT, SIG_DFL);
    execvp(...);
}
```

Or, preferably, use `sigaction()`.

---

# 9. For your shell, the recommended setup

Given that you're implementing pipelines, background processes, `activities`, and job control, I'd structure it as:

```text
main()
 │
 ├── establish shell process group
 │       setpgid()
 │
 ├── take control of terminal
 │       tcsetpgrp()
 │
 ├── shell ignores:
 │       SIGINT
 │       SIGTSTP
 │       SIGQUIT
 │       SIGTTIN
 │       SIGTTOU
 │
 ├── install SIGCHLD handler
 │
 └── shell loop
         │
         ├── foreground job
         │      ├── fork children
         │      ├── setpgid()
         │      ├── dup2()
         │      ├── execvp()
         │      ├── tcsetpgrp(job_pgid)
         │      ├── wait
         │      └── tcsetpgrp(shell_pgid)
         │
         └── background job
                ├── fork children
                ├── setpgid()
                ├── dup2()
                ├── execvp()
                └── DON'T give terminal to job
```

The most important rule is:

> **The shell ignores terminal-generated signals; the child processes restore default signal handling; the terminal is handed to the foreground job's process group.**

That combination is what makes `Ctrl+C` kill a foreground command/pipeline **without killing your shell prompt**, while `Ctrl+Z` stops the foreground job **without stopping the shell itself**.

## You

how does kill pid sig work what happens when you pass a negative PID (⁠ kill(-pgid, sig) ⁠) vs a positive PID

## ChatGPT

`kill()` is slightly confusing because the first argument isn't always simply "the PID to kill."

The basic function is:

```c
kill(pid, sig);
```

It means **send signal `sig` to a process or process group**, depending on the value of `pid`.

---

## 1. Positive PID: one specific process

If:

```c
kill(1234, SIGINT);
```

then Unix sends `SIGINT` to the process whose PID is `1234`.

Conceptually:

```text
Shell
 │
 ├── PID 1234 → SIGINT
 │
 ├── PID 1235
 │
 └── PID 1236
```

Only **PID 1234** receives the signal.

For example:

```c
kill(pid, SIGTERM);
```

means:

> Send SIGTERM to this particular process.

---

# 2. Negative PID: a process group

This is particularly important for your shell.

If you do:

```c
kill(-pgid, SIGINT);
```

the negative value means:

> Send `SIGINT` to **every process in process group `pgid`**.

For example, suppose your pipeline is:

```bash
cat file | grep hello | wc -l
```

and your shell has placed all three processes into:

```text
PGID = 5000
```

You have:

```text
PID     PGID

5000    5000    cat
5001    5000    grep
5002    5000    wc
```

Then:

```c
kill(-5000, SIGINT);
```

does:

```text
SIGINT
  │
  ▼
PGID 5000
  ├── cat
  ├── grep
  └── wc
```

All three receive `SIGINT`.

That's why process groups are so useful for shell job control.

---

# 3. Why is the minus sign significant?

The kernel interprets the first argument roughly like this:

```text
pid > 0
    → send to process with that PID

pid < 0
    → send to every process whose PGID = -pid

pid == 0
    → send to every process in caller's process group

pid == -1
    → send to all processes the caller is permitted to signal
```

So:

```c
kill(1234, SIGINT);
```

means:

```text
PID 1234
```

whereas:

```c
kill(-1234, SIGINT);
```

means:

```text
process group 1234
```

The actual process-group ID is `1234`, not `-1234`.

---

# 4. `kill(0, sig)`

There's another useful case:

```c
kill(0, SIGINT);
```

This sends the signal to **all processes in the caller's process group**.

For example:

```text
PGID 1000
 ├── shell
 ├── process A
 └── process B
```

If the shell calls:

```c
kill(0, SIGINT);
```

all processes in PGID 1000 are targeted.

This is why you should be careful with `kill(0, ...)` in a shell.

---

# 5. `kill(-1, sig)`

This has a different meaning:

```c
kill(-1, SIGTERM);
```

It targets **all processes the caller has permission to signal**, with some system-defined exclusions such as the calling process itself.

You generally **do not want this in a shell** unless you very specifically intend that behavior.

---

# 6. Why shells use negative PIDs

Suppose you have:

```bash
sleep 100 | grep x &
```

Your shell creates:

```text
Job 1
PGID = 7000

PID 7000 → sleep
PID 7001 → grep
```

If you want to terminate the entire job, doing:

```c
kill(7000, SIGTERM);
```

only targets:

```text
sleep
```

`grep` continues running.

Instead:

```c
kill(-7000, SIGTERM);
```

targets:

```text
PGID 7000
 ├── sleep
 └── grep
```

So **both processes receive the signal**.

This is exactly what you want for shell commands that are pipelines.

---

# 7. `SIGINT`, `SIGTSTP`, `SIGCONT`

This becomes especially useful for job control.

### Ctrl+C / terminate foreground job

```c
kill(-pgid, SIGINT);
```

Targets the entire process group.

### Stop a job

```c
kill(-pgid, SIGTSTP);
```

Stops all processes in the group.

### Resume a stopped job

```c
kill(-pgid, SIGCONT);
```

Continues all processes in the group.

For example, your `fg` implementation might eventually do:

```c
kill(-job->pgid, SIGCONT);
tcsetpgrp(STDIN_FILENO, job->pgid);
```

---

# 8. `kill()` does not necessarily "kill"

The name is misleading.

```c
kill(pid, sig);
```

really means:

> **Send signal `sig` to the specified process/process group.**

For example:

```c
kill(pid, SIGSTOP);
```

doesn't terminate the process. It stops it.

```c
kill(pid, SIGCONT);
```

resumes it.

```c
kill(pid, SIGINT);
```

usually terminates it, unless the process has changed its `SIGINT` handling.

```c
kill(pid, SIGTERM);
```

requests termination.

So think:

```text
kill() = send a signal
```

not:

```text
kill() = always terminate
```

---

# 9. How this relates to Ctrl+C in your shell

Normally, your shell should **not need to call `kill()` itself when the user presses Ctrl+C**.

If you've correctly implemented job control:

```text
                 Terminal
                    │
                    │ Ctrl+C
                    ▼
                  SIGINT
                    │
                    ▼
          foreground process group
                    │
             ┌──────┼──────┐
             ▼      ▼      ▼
            cat    grep    wc
```

The terminal automatically sends `SIGINT` to the foreground process group.

Your shell can ignore `SIGINT`:

```c
sigaction(SIGINT, &ignore_action, NULL);
```

while the children restore:

```c
sigaction(SIGINT, &default_action, NULL);
```

---

# 10. One important distinction

These two approaches can both result in multiple processes receiving a signal, but they happen differently:

### Terminal-generated signal

```text
Ctrl+C
   ↓
terminal driver
   ↓
foreground PGID
   ↓
all processes in group
```

### Shell explicitly calling `kill()`

```c
kill(-pgid, SIGINT);
```

```text
shell
  ↓
kernel
  ↓
all processes in PGID
```

For a properly implemented interactive shell, **terminal job control should handle Ctrl+C/Ctrl+Z automatically**. You use `kill(-pgid, ...)` when your shell itself needs to control a job, such as implementing `fg`, `bg`, or a `kill` built-in.

---

## Quick cheat sheet

```c
kill(1234, sig);
```

→ signal **one process**, PID 1234.

```c
kill(-1234, sig);
```

→ signal **every process in process group 1234**.

```c
kill(0, sig);
```

→ signal **every process in the caller's process group**.

```c
kill(-1, sig);
```

→ signal **all permitted processes** (subject to kernel exclusions).

For your shell, the one you'll use most for job control is:

```c
kill(-job->pgid, SIGCONT);   // resume entire job
```

and potentially:

```c
kill(-job->pgid, SIGTERM);   // terminate entire job
```

because **a pipeline should generally be treated as one job/process group, not as a collection of unrelated PIDs**.

## You

How can high-resolution timestamps be recorded across syscall entry and exit to compute per-syscall execution latency?

## ChatGPT

If you're doing this in **xv6-riscv**, the clean way is to record a timestamp at **syscall entry**, record another at **syscall exit**, and subtract them:

```text
syscall entry
     │
     ▼
start = timestamp()
     │
     │ execute syscall
     ▼
result = syscall()
     │
     ▼
end = timestamp()
     │
     ▼
latency = end - start
```

The important question is what clock/source you use.

## 1. Use the RISC-V cycle counter

On RISC-V, `rdcycle` gives you the current cycle counter. In xv6, you can expose a small helper:

```c
static inline uint64
read_cycle(void)
{
    uint64 x;
    asm volatile("rdcycle %0" : "=r" (x));
    return x;
}
```

Then:

```c
uint64 start = read_cycle();

/* syscall work */

uint64 end = read_cycle();

uint64 latency = end - start;
```

This gives you latency in **CPU cycles**, not nanoseconds.

For example:

```text
start = 10,000,000
end   = 10,000,850

latency = 850 cycles
```

---

# 2. Where to put it in xv6

In xv6-riscv, syscall dispatch happens in `kernel/syscall.c`, in the `syscall()` function.

Conceptually, it looks like:

```c
void
syscall(void)
{
    int num;
    struct proc *p = myproc();

    num = p->trapframe->a7;

    if(num > 0 && num < NELEM(syscalls) &&
       syscalls[num]) {

        p->trapframe->a0 = syscalls[num]();

    } else {
        printf("%d %s: unknown sys call %d\n",
               p->pid, p->name, num);

        p->trapframe->a0 = -1;
    }
}
```

You can instrument it like this:

```c
void
syscall(void)
{
    int num;
    uint64 start, end;
    struct proc *p = myproc();

    num = p->trapframe->a7;

    if(num > 0 && num < NELEM(syscalls) &&
       syscalls[num]) {

        start = read_cycle();

        p->trapframe->a0 = syscalls[num]();

        end = read_cycle();

        printf("pid=%d syscall=%d latency=%lu cycles\n",
               p->pid,
               num,
               end - start);

    } else {
        printf("%d %s: unknown sys call %d\n",
               p->pid, p->name, num);

        p->trapframe->a0 = -1;
    }
}
```

This measures:

```text
                    start                  end
                      │                      │
                      ▼                      ▼
syscall trap → syscall handler → syscalls[num]() → return
                                  │
                                  │
                             measured region
```

So `end - start` is the execution time of the syscall handler/function.

---

# 3. But there is an important distinction: entry/exit

If your assignment specifically asks for timestamps at **syscall entry and syscall exit**, it can be useful to put the timestamps at the boundaries of the syscall dispatcher:

```c
void
syscall(void)
{
    uint64 start, end;
    int num;
    struct proc *p = myproc();

    start = read_cycle();

    num = p->trapframe->a7;

    if(num > 0 && num < NELEM(syscalls) &&
       syscalls[num]) {
        p->trapframe->a0 = syscalls[num]();
    } else {
        p->trapframe->a0 = -1;
    }

    end = read_cycle();

    ...
}
```

This measures approximately:

```text
trap enters syscall()
        │
        ▼
      START
        │
        ├── identify syscall
        ├── execute syscall
        └── set return value
        │
        ▼
       END
        │
        ▼
return to user
```

That's generally what you'd want for **per-syscall latency**.

---

# 4. Don't use `printf()` between the timestamps

Avoid:

```c
uint64 start = read_cycle();

printf("starting syscall\n");

syscalls[num]();

printf("finished syscall\n");

uint64 end = read_cycle();
```

because you're now measuring the cost of the `printf()` calls too.

Instead:

```c
uint64 start = read_cycle();

uint64 result = syscalls[num]();

uint64 end = read_cycle();
```

Then print the result **after** you've captured the end timestamp.

Even better, if you're doing performance measurements, store the measurements and print them later.

---

# 5. Store per-syscall statistics

If you want more than one measurement, maintain something like:

```c
#define NSYSCALLS 64

struct syscall_stats {
    uint64 count;
    uint64 total_cycles;
    uint64 min_cycles;
    uint64 max_cycles;
};

struct syscall_stats stats[NSYSCALLS];
```

Then:

```c
uint64 start = read_cycle();

p->trapframe->a0 = syscalls[num]();

uint64 end = read_cycle();

uint64 elapsed = end - start;

stats[num].count++;
stats[num].total_cycles += elapsed;

if(stats[num].count == 1 ||
   elapsed < stats[num].min_cycles)
    stats[num].min_cycles = elapsed;

if(elapsed > stats[num].max_cycles)
    stats[num].max_cycles = elapsed;
```

Now you can calculate:

```text
average latency =
    total_cycles / count
```

and have:

```text
syscall     count     min       max       average
---------------------------------------------------
read          50      320      1800        540
write         20      400      2100        720
fork           5     5000     12000       7800
```

---

# 6. Cycles vs real time

`rdcycle` gives you cycles.

If the CPU frequency is known and constant:

```text
time = cycles / frequency
```

For example, if:

```text
CPU frequency = 1 GHz
latency        = 1000 cycles
```

then:

```text
1000 / 1,000,000,000 seconds
= 1 microsecond
```

But for xv6/QEMU experiments, **cycles are often preferable** because they avoid assuming a particular wall-clock frequency.

If you specifically need nanoseconds, you'd need to use an appropriate timer source and know its frequency/conversion.

---

# 7. Important issue: interrupts and scheduling

The measured interval can include more than the pure execution instructions of the syscall.

For example:

```text
START
  │
  ▼
syscall starts
  │
  │ ← timer interrupt
  │
  │ ← process gets descheduled
  │
  │ ← process runs again
  │
  ▼
END
```

Then:

```text
end - start
```

includes the time during which the process wasn't actually executing.

So distinguish between:

### Wall-clock-style syscall latency

```text
end_timestamp - start_timestamp
```

Includes interruptions/preemption.

### CPU execution time

Would require accounting for scheduling/descheduling and is more complicated.

For most xv6 syscall-latency assignments, **entry timestamp → syscall return timestamp** is the expected measurement.

---

# 8. Per-process storage is often cleaner

If you don't want global statistics, add fields to `struct proc`:

```c
struct proc {
    ...
    uint64 syscall_start;
    uint64 syscall_end;
    uint64 syscall_latency;
    ...
};
```

Then:

```c
p->syscall_start = read_cycle();

p->trapframe->a0 = syscalls[num]();

p->syscall_end = read_cycle();

p->syscall_latency =
    p->syscall_end - p->syscall_start;
```

This is particularly useful if another syscall such as:

```text
get_syscall_stats()
```

needs to return the information to a user program.

---

# 9. If you're implementing syscall tracing too

You might already have something like:

```c
printf("%d: syscall %s\n", p->pid, syscall_name[num]);
```

You can combine tracing and timing:

```c
uint64 start = read_cycle();

p->trapframe->a0 = syscalls[num]();

uint64 end = read_cycle();

printf("pid=%d syscall=%s cycles=%lu\n",
       p->pid,
       syscall_names[num],
       end - start);
```

For example:

```text
pid=3 syscall=read cycles=842
pid=3 syscall=write cycles=911
pid=3 syscall=fork cycles=7342
```

---

## The core implementation

For an xv6 syscall-latency measurement, the essential code is just:

```c
uint64 start, end;

start = read_cycle();

p->trapframe->a0 = syscalls[num]();

end = read_cycle();

uint64 latency = end - start;
```

with:

```c
static inline uint64
read_cycle(void)
{
    uint64 x;
    asm volatile("rdcycle %0" : "=r"(x));
    return x;
}
```

The conceptual placement is:

```text
                 syscall entry
                      │
                      ▼
              ┌───────────────┐
              │ start=rdcycle │
              └───────┬───────┘
                      │
                      ▼
               identify syscall
                      │
                      ▼
                syscalls[num]()
                      │
                      ▼
              ┌───────────────┐
              │  end=rdcycle  │
              └───────┬───────┘
                      │
                      ▼
              end - start
                      │
                      ▼
               syscall latency
```

For **xv6-riscv**, I'd recommend starting with `rdcycle` and storing `count`, `total`, `min`, and `max` per syscall if your assignment asks for meaningful latency statistics rather than just printing one timestamp pair.

## You

Write a C program ⁠ snoop ⁠ that either forks and traces a commandor attaches to a running PID . Step through all syscalls and print a summary table upon exit.

## ChatGPT

If this is for a **Linux C-shell / systems assignment**, a practical way to implement `snoop` is with Linux's `ptrace(PTRACE_SYSCALL, ...)`.

The program below supports both:

```bash
./snoop ls -l
```

and:

```bash
./snoop -p 12345
```

It stops the traced process at **every syscall entry and exit**, measures the time spent between the two stops, counts each syscall, and prints a summary when the process exits.

> This version targets **Linux x86-64**, where the syscall number is in `orig_rax` and the return value is in `rax`.

:::writing{variant="document" id="61483" title="snoop — Linux syscall tracer"}
```c
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>

/*
 * Linux x86-64 syscall numbers.
 * Add more names here if your assignment requires them.
 */
static const char *syscall_name(long nr)
{
    switch (nr) {
        case 0:   return "read";
        case 1:   return "write";
        case 2:   return "open";
        case 3:   return "close";
        case 4:   return "stat";
        case 5:   return "fstat";
        case 6:   return "lstat";
        case 7:   return "poll";
        case 8:   return "lseek";
        case 9:   return "mmap";
        case 10:  return "mprotect";
        case 11:  return "munmap";
        case 12:  return "brk";
        case 13:  return "rt_sigaction";
        case 14:  return "rt_sigprocmask";
        case 15:  return "rt_sigreturn";
        case 16:  return "ioctl";
        case 17:  return "pread64";
        case 18:  return "pwrite64";
        case 19:  return "readv";
        case 20:  return "writev";
        case 21:  return "access";
        case 22:  return "pipe";
        case 23:  return "select";
        case 24:  return "sched_yield";
        case 25:  return "mremap";
        case 28:  return "madvise";
        case 32:  return "dup";
        case 33:  return "dup2";
        case 35:  return "nanosleep";
        case 39:  return "getpid";
        case 41:  return "socket";
        case 42:  return "connect";
        case 43:  return "accept";
        case 44:  return "sendto";
        case 45:  return "recvfrom";
        case 46:  return "sendmsg";
        case 47:  return "recvmsg";
        case 48:  return "shutdown";
        case 49:  return "bind";
        case 50:  return "listen";
        case 56:  return "clone";
        case 57:  return "fork";
        case 58:  return "vfork";
        case 59:  return "execve";
        case 60:  return "exit";
        case 61:  return "wait4";
        case 62:  return "kill";
        case 63:  return "uname";
        case 72:  return "fcntl";
        case 79:  return "getcwd";
        case 80:  return "chdir";
        case 83:  return "mkdir";
        case 87:  return "unlink";
        case 89:  return "readlink";
        case 90:  return "chmod";
        case 91:  return "fchmod";
        case 92:  return "chown";
        case 96:  return "gettimeofday";
        case 97:  return "getrlimit";
        case 98:  return "getrusage";
        case 99:  return "sysinfo";
        case 102: return "getuid";
        case 104: return "getgid";
        case 105: return "setuid";
        case 106: return "setgid";
        case 107: return "geteuid";
        case 108: return "getegid";
        case 110: return "getppid";
        case 131: return "sigaltstack";
        case 158: return "arch_prctl";
        case 186: return "gettid";
        case 202: return "futex";
        case 217: return "getdents64";
        case 218: return "set_tid_address";
        case 228: return "clock_gettime";
        case 231: return "exit_group";
        case 232: return "epoll_wait";
        case 233: return "epoll_ctl";
        case 257: return "openat";
        case 258: return "mkdirat";
        case 262: return "newfstatat";
        case 263: return "unlinkat";
        case 267: return "readlinkat";
        case 269: return "fchmodat";
        case 271: return "ppoll";
        case 273: return "set_robust_list";
        case 280: return "utimensat";
        case 281: return "epoll_pwait";
        case 288: return "accept4";
        case 293: return "pipe2";
        case 302: return "prlimit64";
        case 318: return "getrandom";
        case 319: return "memfd_create";
        case 322: return "execveat";
        case 332: return "statx";
        default:  return "unknown";
    }
}

#define MAX_SYSCALLS 512

struct syscall_stat {
    unsigned long long count;
    unsigned long long total_ns;
    unsigned long long min_ns;
    unsigned long long max_ns;
};

static struct syscall_stat stats[MAX_SYSCALLS];


/*
 * Return monotonic time in nanoseconds.
 *
 * CLOCK_MONOTONIC is preferable for latency measurements because
 * it isn't affected by changes to wall-clock time.
 */
static unsigned long long now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return (unsigned long long)ts.tv_sec * 1000000000ULL
           + ts.tv_nsec;
}


/*
 * Record one syscall's execution time.
 */
static void record_syscall(long nr,
                           unsigned long long elapsed)
{
    if (nr < 0 || nr >= MAX_SYSCALLS)
        return;

    stats[nr].count++;
    stats[nr].total_ns += elapsed;

    if (stats[nr].count == 1 ||
        elapsed < stats[nr].min_ns) {
        stats[nr].min_ns = elapsed;
    }

    if (elapsed > stats[nr].max_ns) {
        stats[nr].max_ns = elapsed;
    }
}


/*
 * Print the final summary.
 */
static void print_summary(void)
{
    printf("\n");
    printf("===============================================================\n");
    printf("                         SYSCALL SUMMARY\n");
    printf("===============================================================\n");

    printf("%-5s %-24s %10s %14s %14s %14s\n",
           "No.",
           "Syscall",
           "Count",
           "Total (us)",
           "Avg (us)",
           "Max (us)");

    printf("---------------------------------------------------------------\n");

    unsigned long long total_calls = 0;

    for (int i = 0; i < MAX_SYSCALLS; i++) {

        if (stats[i].count == 0)
            continue;

        double total_us =
            stats[i].total_ns / 1000.0;

        double avg_us =
            (double)stats[i].total_ns /
            stats[i].count / 1000.0;

        double max_us =
            stats[i].max_ns / 1000.0;

        printf("%-5d %-24s %10llu %14.3f %14.3f %14.3f\n",
               i,
               syscall_name(i),
               stats[i].count,
               total_us,
               avg_us,
               max_us);

        total_calls += stats[i].count;
    }

    printf("---------------------------------------------------------------\n");
    printf("Total syscalls: %llu\n", total_calls);
    printf("===============================================================\n");
}


/*
 * Trace an already-created child.
 *
 * The child has already called PTRACE_TRACEME and stopped itself.
 */
static void trace_process(pid_t pid)
{
    int status;

    /*
     * Wait for the initial SIGSTOP from the child.
     */
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        exit(EXIT_FAILURE);
    }

    /*
     * Tell ptrace that we want syscall stops to be distinguishable
     * from ordinary SIGTRAP stops.
     */
    if (ptrace(PTRACE_SETOPTIONS,
               pid,
               0,
               PTRACE_O_TRACESYSGOOD) == -1) {

        perror("PTRACE_SETOPTIONS");
        exit(EXIT_FAILURE);
    }

    /*
     * Alternate between:
     *
     *     syscall entry
     *     syscall exit
     *
     * PTRACE_SYSCALL causes the child to continue until the next
     * syscall entry/exit stop.
     */
    int entering = 1;

    long current_syscall = -1;
    unsigned long long start_time = 0;

    while (1) {

        if (ptrace(PTRACE_SYSCALL, pid, 0, 0) == -1) {

            if (errno == ESRCH)
                break;

            perror("PTRACE_SYSCALL");
            break;
        }

        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            break;
        }

        /*
         * Process exited normally.
         */
        if (WIFEXITED(status)) {
            break;
        }

        /*
         * Process was killed by a signal.
         */
        if (WIFSIGNALED(status)) {
            break;
        }

        /*
         * We only care about stopped processes from here.
         */
        if (!WIFSTOPPED(status))
            continue;

        int sig = WSTOPSIG(status);

        /*
         * PTRACE_O_TRACESYSGOOD causes syscall stops to have
         * bit 7 set in the signal number.
         */
        if (sig == (SIGTRAP | 0x80)) {

            struct user_regs_struct regs;

            if (ptrace(PTRACE_GETREGS,
                       pid,
                       0,
                       &regs) == -1) {

                perror("PTRACE_GETREGS");
                break;
            }

            if (entering) {

                /*
                 * x86-64:
                 * orig_rax contains the syscall number at entry.
                 */
                current_syscall = regs.orig_rax;

                start_time = now_ns();

                /*
                 * Optional live tracing:
                 *
                 * printf("ENTER %-20s\n",
                 *        syscall_name(current_syscall));
                 */

                entering = 0;

            } else {

                /*
                 * We are at syscall exit.
                 */
                unsigned long long end_time = now_ns();

                unsigned long long elapsed =
                    end_time - start_time;

                record_syscall(current_syscall,
                               elapsed);

                /*
                 * Optional live tracing:
                 *
                 * printf("EXIT  %-20s %llu ns\n",
                 *        syscall_name(current_syscall),
                 *        elapsed);
                 */

                entering = 1;
            }

        } else {

            /*
             * This was not a syscall-stop.
             *
             * It may be an ordinary signal delivered to the
             * traced process.
             *
             * Pass it through so the traced program behaves
             * normally.
             */
            if (sig != SIGTRAP) {

                if (ptrace(PTRACE_SYSCALL,
                           pid,
                           0,
                           sig) == -1) {

                    perror("PTRACE_SYSCALL signal");
                    break;
                }

                if (waitpid(pid, &status, 0) == -1)
                    break;

                if (WIFEXITED(status) ||
                    WIFSIGNALED(status))
                    break;
            }
        }
    }
}


/*
 * Fork and execute a new command.
 *
 * Example:
 *
 *     ./snoop ls -l
 */
static void trace_command(char *argv[])
{
    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {

        /*
         * Tell the kernel that the parent will trace us.
         */
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) == -1) {
            perror("PTRACE_TRACEME");
            _exit(1);
        }

        /*
         * Stop ourselves so the parent can set up ptrace.
         */
        raise(SIGSTOP);

        /*
         * Replace child with requested command.
         */
        execvp(argv[0], argv);

        perror("execvp");
        _exit(127);
    }

    /*
     * Parent becomes the tracer.
     */
    trace_process(pid);
}


/*
 * Attach to an already-running process.
 *
 * Example:
 *
 *     ./snoop -p 12345
 */
static void trace_pid(pid_t pid)
{
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) == -1) {
        perror("PTRACE_ATTACH");
        exit(EXIT_FAILURE);
    }

    /*
     * PTRACE_ATTACH causes the target to stop.
     */
    trace_process(pid);
}


static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s command [args...]\n"
            "  %s -p PID\n",
            prog, prog);

    exit(EXIT_FAILURE);
}


int main(int argc, char *argv[])
{
    if (argc < 2)
        usage(argv[0]);

    /*
     * Initialize statistics.
     */
    memset(stats, 0, sizeof(stats));

    /*
     * Attach mode:
     *
     *     snoop -p 1234
     */
    if (strcmp(argv[1], "-p") == 0) {

        if (argc != 3)
            usage(argv[0]);

        char *end;
        long pid_long = strtol(argv[2], &end, 10);

        if (*end != '\0' ||
            pid_long <= 0) {

            fprintf(stderr, "Invalid PID: %s\n", argv[2]);
            return EXIT_FAILURE;
        }

        trace_pid((pid_t)pid_long);

    } else {

        /*
         * Fork/exec mode:
         *
         *     snoop ls -l
         */
        trace_command(&argv[1]);
    }

    /*
     * Print statistics after the traced program exits.
     */
    print_summary();

    return 0;
}
```
:::

### Compile

```bash
gcc -Wall -Wextra -O2 snoop.c -o snoop
```

Run a new command:

```bash
./snoop ls
```

or:

```bash
./snoop cat /etc/hosts
```

Attach to an existing process:

```bash
./snoop -p 12345
```

Depending on Linux security settings, attaching to another user's process may require appropriate privileges.

---

## How the tracing actually works

The important part is:

```c
ptrace(PTRACE_SYSCALL, pid, 0, 0);
```

It tells the kernel:

> Continue this process, but stop it whenever it enters or exits a system call.

So for:

```c
read(fd, buf, 100);
```

the tracer sees approximately:

```text
             traced process
                  │
                  │ read()
                  ▼
             syscall entry
                  │
                  │ ptrace stop
                  ▼
               snoop
                  │
                  │ record start time
                  ▼
          PTRACE_SYSCALL
                  │
                  ▼
             kernel executes
                  │
                  ▼
             syscall exit
                  │
                  │ ptrace stop
                  ▼
               snoop
                  │
                  │ record end time
                  ▼
            latency = end-start
```

That's why the code has:

```c
int entering = 1;
```

and alternates:

```c
if (entering) {
    // syscall entry
    start_time = now_ns();
    entering = 0;
}
else {
    // syscall exit
    end_time = now_ns();
    record_syscall(...);
    entering = 1;
}
```

---

## Where the syscall number comes from

On **x86-64 Linux**, `ptrace()` exposes the CPU registers through:

```c
struct user_regs_struct regs;
```

At syscall entry:

```c
regs.orig_rax
```

contains the syscall number.

For example:

```text
orig_rax = 0
```

means:

```text
read()
```

and:

```text
orig_rax = 1
```

means:

```text
write()
```

So:

```c
current_syscall = regs.orig_rax;
```

lets you identify the syscall.

The return value is available in:

```c
regs.rax
```

at syscall exit.

If you want to distinguish successful and failed syscalls, you could add:

```c
long return_value = regs.rax;
```

and record that as well.

---

## Why `CLOCK_MONOTONIC`?

The timestamp function uses:

```c
clock_gettime(CLOCK_MONOTONIC, &ts);
```

rather than `time()`.

`time()` only has coarse resolution and measures wall-clock time.

`CLOCK_MONOTONIC` is designed for elapsed-time measurements and gives nanosecond-resolution timestamps:

```text
start = CLOCK_MONOTONIC
        ↓
       syscall
        ↓
end   = CLOCK_MONOTONIC

latency = end - start
```

One caveat: the measured latency includes time in which the traced process may be **descheduled**. So it is syscall **elapsed latency**, not necessarily pure CPU execution time.

---

## Example output

For:

```bash
./snoop echo hello
```

you could get:

```text
===============================================================
                         SYSCALL SUMMARY
===============================================================
No.   Syscall                       Count     Total (us)      Avg (us)      Max (us)
---------------------------------------------------------------
1     write                             1          3.214          3.214          3.214
9     mmap                              8         12.431          1.554          4.812
10    mprotect                          3          2.891          0.964          1.231
11    munmap                            2          1.203          0.602          0.712
12    brk                               4          2.104          0.526          0.811
13    rt_sigaction                      5          1.902          0.380          0.521
59    execve                            1         15.421         15.421         15.421
60    exit                              1          0.000          0.000          0.000
---------------------------------------------------------------
Total syscalls: 25
===============================================================
```

The exact numbers will vary.

### One limitation you should know

This implementation traces the **initial process**. If the traced program itself creates children using `fork()`/`clone()`, those children aren't automatically included. For a full `strace`-style implementation, you'd add:

```c
PTRACE_O_TRACEFORK
PTRACE_O_TRACEVFORK
PTRACE_O_TRACECLONE
```

and maintain a set of traced PIDs.

For a basic `snoop` assignment, though, the architecture above gives you the essential pieces: **fork/attach → `ptrace` → syscall entry/exit → timestamp → per-syscall statistics → final summary**.

## You

How do I safely handle ⁠ sigint in a ⁠ ptrace ⁠ tracer attached to a pid so that it detaches with ⁠ptrace detach , restores the tracee, and cleanly prints accumulated syscall statistics?

## ChatGPT

For a `ptrace` tracer, you don't want the tracer itself to die when you press **Ctrl+C**. Instead, the tracer should:

1. Catch `SIGINT`.
2. Set a flag in the handler.
3. Let the main tracing loop notice the flag.
4. Stop/resume the tracee as necessary.
5. Call `ptrace(PTRACE_DETACH, ...)`.
6. Make sure the tracee is left in a normal runnable state.
7. Print the accumulated statistics.
8. Exit.

The important rule is: **keep the signal handler minimal**. Don't call `printf()`, `ptrace()`, or other complicated functions directly from the `SIGINT` handler.

---

## 1. Use a `sig_atomic_t` flag

```c
#include <signal.h>

static volatile sig_atomic_t interrupted = 0;

static void sigint_handler(int sig)
{
    (void)sig;
    interrupted = 1;
}
```

Install it before starting the tracing:

```c
struct sigaction sa;

sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;
sa.sa_handler = sigint_handler;

if (sigaction(SIGINT, &sa, NULL) == -1) {
    perror("sigaction");
    exit(EXIT_FAILURE);
}
```

Now:

```text
Ctrl+C
  │
  ▼
SIGINT
  │
  ▼
sigint_handler()
  │
  ▼
interrupted = 1
```

The handler does **not** try to detach immediately.

---

# 2. Why shouldn't the handler call `ptrace()`?

Avoid:

```c
void sigint_handler(int sig)
{
    ptrace(PTRACE_DETACH, ...);  // DON'T
    printf("Stopping...\n");     // DON'T
}
```

Signal handlers execute asynchronously and have restrictions on what functions they can safely call.

Instead:

```c
static volatile sig_atomic_t interrupted;

void sigint_handler(int sig)
{
    interrupted = 1;
}
```

Then your normal code handles cleanup.

---

# 3. The problem with a tracee stopped inside `waitpid()`

Your tracer is typically doing:

```c
ptrace(PTRACE_SYSCALL, pid, 0, 0);

waitpid(pid, &status, 0);
```

When you press Ctrl+C:

```text
tracer
  │
  └── waitpid()
       │
       │ Ctrl+C
       ▼
     SIGINT
       │
       ▼
interrupted = 1
```

Depending on how `waitpid()` and your `sigaction()` flags are configured, `waitpid()` may return `-1` with:

```c
errno == EINTR
```

So your loop needs to recognize this:

```c
pid_t r = waitpid(pid, &status, 0);

if (r == -1) {
    if (errno == EINTR && interrupted)
        break;

    perror("waitpid");
    break;
}
```

If you use `SA_RESTART`, `waitpid()` may be automatically restarted instead, which makes this pattern less convenient for an interruptible tracer.

For this particular program, I would **not use `SA_RESTART` for SIGINT**.

---

# 4. Safely detach

Once your main loop notices:

```c
interrupted == 1
```

you need to ensure the tracee isn't left stopped under ptrace.

A simple cleanup function is:

```c
static void detach_tracee(pid_t pid)
{
    /*
     * If the tracee is currently stopped because of ptrace,
     * PTRACE_DETACH will resume it.
     */
    if (ptrace(PTRACE_DETACH, pid, 0, 0) == -1) {

        if (errno != ESRCH) {
            perror("PTRACE_DETACH");
        }
    }
}
```

The important behavior is:

```text
PTRACE_DETACH
      │
      ├── removes ptrace relationship
      │
      └── resumes tracee
```

So the target process continues running normally.

---

# 5. The subtle case: tracee is running

This is the part that causes many beginner implementations to fail.

`PTRACE_DETACH` generally needs the tracee to be in a ptrace-stop.

If the tracee is currently running, you can't blindly assume:

```c
ptrace(PTRACE_DETACH, pid, 0, 0);
```

will work.

A useful cleanup strategy is:

```text
Tracer receives SIGINT
       │
       ▼
request/ensure tracee stops
       │
       ▼
wait for tracee's stop
       │
       ▼
PTRACE_DETACH
       │
       ▼
tracee resumes normally
```

For a tracer that is currently stopped at a syscall-stop, you're already in the appropriate state.

If you need to force a running tracee to stop during cleanup, one approach is:

```c
kill(pid, SIGSTOP);
```

then:

```c
waitpid(pid, &status, 0);
```

and finally:

```c
ptrace(PTRACE_DETACH, pid, 0, 0);
```

But there is an important distinction between **a ptrace-stop** and an ordinary `SIGSTOP`, so don't blindly send signals in every state. The cleanest design is to track whether your tracer currently has the tracee stopped.

---

# 6. Modify your tracing loop

For your `snoop -p PID` implementation, structure the loop approximately like this:

```c
static void trace_process(pid_t pid)
{
    int status;
    int entering = 1;

    long current_syscall = -1;
    unsigned long long start_time = 0;

    while (!interrupted) {

        /*
         * Ask tracee to continue until next syscall stop.
         */
        if (ptrace(PTRACE_SYSCALL, pid, 0, 0) == -1) {

            if (errno == ESRCH)
                break;

            perror("PTRACE_SYSCALL");
            break;
        }

        /*
         * Wait for the tracee.
         */
        pid_t r = waitpid(pid, &status, 0);

        if (r == -1) {

            if (errno == EINTR && interrupted)
                break;

            perror("waitpid");
            break;
        }

        /*
         * Tracee exited normally.
         */
        if (WIFEXITED(status)) {
            break;
        }

        /*
         * Tracee was killed.
         */
        if (WIFSIGNALED(status)) {
            break;
        }

        if (!WIFSTOPPED(status))
            continue;

        /*
         * Syscall stop.
         */
        if (WSTOPSIG(status) == (SIGTRAP | 0x80)) {

            struct user_regs_struct regs;

            if (ptrace(PTRACE_GETREGS,
                       pid,
                       0,
                       &regs) == -1) {

                perror("PTRACE_GETREGS");
                break;
            }

            if (entering) {

                current_syscall = regs.orig_rax;
                start_time = now_ns();

                entering = 0;

            } else {

                unsigned long long end_time = now_ns();

                record_syscall(
                    current_syscall,
                    end_time - start_time
                );

                entering = 1;
            }
        }
    }

    /*
     * We get here when Ctrl+C was pressed or
     * the tracee terminated.
     */
    if (interrupted) {
        detach_tracee(pid);
    }
}
```

Then, after `trace_process()` returns:

```c
print_summary();
```

---

# 7. What happens when you press Ctrl+C?

Suppose:

```bash
./snoop -p 12345
```

and PID 12345 is currently executing:

```text
PID 12345
     │
     └── read()
```

Your tracer is waiting:

```text
snoop
 │
 └── waitpid(12345)
```

You press Ctrl+C.

The flow becomes:

```text
             Ctrl+C
                │
                ▼
             SIGINT
                │
                ▼
       snoop SIGINT handler
                │
                ▼
        interrupted = 1
                │
                ▼
          tracing loop exits
                │
                ▼
       PTRACE_DETACH(12345)
                │
                ▼
       tracee continues running
                │
                ▼
          print_summary()
                │
                ▼
              exit
```

The crucial point is:

> **Ctrl+C terminates the tracer, not the tracee.**

That's what you want when `snoop -p PID` means "observe this existing process."

---

# 8. Don't send SIGINT to the tracee

This is a common mistake.

Don't do:

```c
kill(pid, SIGINT);
```

when the user presses Ctrl+C.

That would ask the **tracee** to handle SIGINT, potentially terminating it.

Instead:

```text
Ctrl+C
  │
  ▼
SIGINT → snoop
          │
          ▼
       detach
          │
          ▼
      tracee continues
```

The tracer's SIGINT handler should only set the flag.

---

# 9. What if you're tracing a command you started yourself?

There's a distinction between:

```bash
./snoop -p 12345
```

and:

```bash
./snoop ls
```

For:

```bash
./snoop -p 12345
```

the target already existed before `snoop`.

Usually you want:

```text
Ctrl+C
    ↓
detach
    ↓
target keeps running
```

For:

```bash
./snoop ls
```

you might reasonably want Ctrl+C to behave like normal shell execution:

```text
Ctrl+C
   ↓
ls receives SIGINT
   ↓
ls terminates
   ↓
snoop cleans up
```

So your cleanup behavior should depend on the mode.

For example:

```c
enum trace_mode {
    ATTACH_MODE,
    EXEC_MODE
};
```

Then:

```c
if (interrupted) {

    if (mode == ATTACH_MODE) {
        detach_tracee(pid);
    } else {
        /*
         * For a child we created, decide whether to
         * pass SIGINT to it or terminate the tracing job.
         */
    }
}
```

---

# 10. Restoring the tracee's signal state

Normally, `PTRACE_DETACH` is enough to remove the ptrace relationship and let the process continue.

You should **not** try to manually restore its registers or signal handlers just because you attached with:

```c
PTRACE_ATTACH
```

`ptrace` doesn't replace the tracee's signal dispositions.

What you need to ensure is:

```text
ptrace relationship removed
+
tracee not left stopped
```

So:

```c
ptrace(PTRACE_DETACH, pid, 0, 0);
```

is the key cleanup operation.

---

# 11. A robust cleanup function

I'd make cleanup explicit:

```c
static void cleanup_tracee(pid_t pid, int tracee_stopped)
{
    if (!tracee_stopped)
        return;

    if (ptrace(PTRACE_DETACH, pid, 0, 0) == -1) {

        if (errno != ESRCH) {
            perror("PTRACE_DETACH");
        }
    }
}
```

Then your main tracing loop keeps track of:

```c
int tracee_stopped = 0;
```

Whenever `waitpid()` tells you:

```c
WIFSTOPPED(status)
```

set:

```c
tracee_stopped = 1;
```

After:

```c
PTRACE_SYSCALL
```

the tracee is running again:

```c
tracee_stopped = 0;
```

Then the cleanup can make the correct decision.

---

# 12. One more issue: interrupted syscalls

Suppose the tracee is blocked in:

```c
read(fd, buf, size);
```

and your tracer receives SIGINT.

If you're attached with ptrace, the tracee may be in a ptrace-stop or syscall-stop depending on exactly when the interrupt happens.

Don't assume:

```text
SIGINT → tracee immediately stopped → detach
```

The tracer should instead treat `SIGINT` as a **request to stop tracing**, then let the normal ptrace state machine reach a state where detaching is safe.

That's why this pattern is better:

```text
SIGINT handler:
    interrupted = 1

NOT:
    ptrace(...)
    kill(...)
    printf(...)
```

---

## Recommended structure for your `snoop`

I'd organize it as:

```text
main()
 │
 ├── parse arguments
 │
 ├── install SIGINT handler
 │
 ├── fork/exec OR PTRACE_ATTACH
 │
 ├── trace_process()
 │      │
 │      ├── PTRACE_SYSCALL
 │      ├── waitpid
 │      ├── entry → timestamp
 │      ├── exit  → timestamp
 │      └── record statistics
 │      │
 │      └── interrupted?
 │              │
 │              ▼
 │         cleanup_tracee()
 │
 ├── print_summary()
 │
 └── exit
```

The essential code is:

```c
static volatile sig_atomic_t interrupted = 0;

static void sigint_handler(int sig)
{
    (void)sig;
    interrupted = 1;
}
```

then in the tracer:

```c
while (!interrupted) {

    if (ptrace(PTRACE_SYSCALL, pid, 0, 0) == -1)
        break;

    if (waitpid(pid, &status, 0) == -1) {

        if (errno == EINTR && interrupted)
            break;

        perror("waitpid");
        break;
    }

    /* process syscall entry/exit */
}
```

and finally:

```c
if (interrupted) {
    /*
     * Ensure the tracee is in a ptrace-stop if necessary,
     * then detach.
     */
    ptrace(PTRACE_DETACH, pid, 0, 0);
}

print_summary();
```

For **`snoop -p PID` specifically**, the most important behavioral requirement is: **Ctrl+C should stop `snoop`, detach from the target, leave the target running, then print the statistics collected so far.**

## You

What extra fields need to be added to ⁠ struct proc ⁠ in ⁠ kernel/proc.h ⁠ for MLFQ
how do I implement scheduler logic in mlfq, give a rough draft

## ChatGPT

If you're modifying **xv6-riscv** to implement an **MLFQ (Multi-Level Feedback Queue)** scheduler, you need two parts:

1. Add scheduling state to `struct proc` in `kernel/proc.h`.
2. Change `scheduler()` so it chooses processes based on their queue/priority and updates their priority based on CPU usage.

I'll assume a typical 3- or 4-level MLFQ where **lower queue number = higher priority**.

---

# 1. Fields to add to `struct proc`

In xv6, `struct proc` already has things like:

```c
struct proc {
  struct spinlock lock;

  enum procstate state;
  void *chan;
  int killed;
  int xstate;
  int pid;

  ...
};
```

Add scheduling-related fields:

```c
// MLFQ scheduling information
int queue_level;       // Which MLFQ queue the process is in
int ticks_used;        // Ticks used in current time slice
int total_ticks;       // Total CPU ticks used
int wait_ticks;        // Number of ticks waiting
```

So conceptually:

```c
struct proc {
  struct spinlock lock;

  enum procstate state;
  void *chan;
  int killed;
  int xstate;
  int pid;

  ...

  // MLFQ fields
  int queue_level;
  int ticks_used;
  int total_ticks;
  int wait_ticks;
};
```

You may need different fields depending on your assignment specification, but these are the useful basics.

---

# 2. What each field means

### `queue_level`

This determines priority:

```text
queue_level = 0 → highest priority
queue_level = 1
queue_level = 2
queue_level = 3 → lowest priority
```

For example:

```text
Q0:  interactive / CPU-light processes
Q1:
Q2:
Q3:  CPU-heavy processes
```

---

### `ticks_used`

How much of its current quantum the process has consumed.

For example, if:

```text
Q0 quantum = 1 tick
```

and the process has run for one tick:

```text
ticks_used = 1
```

It has exhausted its quantum.

---

### `total_ticks`

Total CPU time consumed.

This isn't strictly required for basic MLFQ, but it is useful for statistics/debugging.

---

### `wait_ticks`

How long the process has been waiting in a ready state.

This is useful for **aging**.

For example:

```text
Q3 process waits for 100 ticks
        ↓
promote to Q2
```

Aging prevents low-priority processes from starving.

---

# 3. Define your MLFQ parameters

You can put these in something like `kernel/param.h`:

```c
#define NQUEUE 4

#define Q0_QUANTUM 1
#define Q1_QUANTUM 2
#define Q2_QUANTUM 4
#define Q3_QUANTUM 8

#define BOOST_INTERVAL 100
```

Or use an array:

```c
static int quantum[NQUEUE] = {
    1, 2, 4, 8
};
```

This is easier to use.

The general MLFQ idea is:

```text
Higher priority
      │
      ▼
┌─────────────┐ quantum = 1
│     Q0      │
└─────────────┘
      │
      ▼
┌─────────────┐ quantum = 2
│     Q1      │
└─────────────┘
      │
      ▼
┌─────────────┐ quantum = 4
│     Q2      │
└─────────────┘
      │
      ▼
┌─────────────┐ quantum = 8
│     Q3      │
└─────────────┘
      │
      ▼
Lower priority
```

---

# 4. Initialize the fields

You need to initialize these when a process is created.

Look at `allocproc()` in `kernel/proc.c`.

After allocating a process:

```c
p->state = USED;
```

you can initialize:

```c
p->queue_level = 0;
p->ticks_used = 0;
p->total_ticks = 0;
p->wait_ticks = 0;
```

So every new process starts at the highest-priority queue:

```text
new process
     │
     ▼
    Q0
```

This is a common MLFQ policy.

---

# 5. Scheduler's basic logic

The existing xv6 scheduler is roughly:

```c
for(;;) {

  intr_on();

  for(p = proc; p < &proc[NPROC]; p++) {

    acquire(&p->lock);

    if(p->state == RUNNABLE) {

      p->state = RUNNING;

      c->proc = p;

      swtch(&c->context, &p->context);

      c->proc = 0;
    }

    release(&p->lock);
  }
}
```

The problem is that this essentially scans processes and chooses runnable ones without MLFQ priority.

You want:

```text
Find highest-priority RUNNABLE process
                │
                ▼
          run that process
                │
                ▼
        quantum exhausted?
          /           \
        yes            no
        │              │
        ▼              ▼
   demote queue     continue
```

---

# 6. Rough MLFQ scheduler

A rough implementation could look like:

```c
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;

  for(;;) {

    intr_on();

    /*
     * Search from highest priority queue
     * to lowest priority queue.
     */
    for(int q = 0; q < NQUEUE; q++) {

      p = find_runnable_process(q);

      if(p == 0)
        continue;

      acquire(&p->lock);

      if(p->state != RUNNABLE) {
        release(&p->lock);
        continue;
      }

      p->state = RUNNING;
      c->proc = p;

      /*
       * Reset the time used for this scheduling period.
       */
      p->ticks_used = 0;

      swtch(&c->context, &p->context);

      c->proc = 0;

      release(&p->lock);

      /*
       * Once we run a process, restart the
       * priority search.
       */
      break;
    }
  }
}
```

The exact `find_runnable_process()` implementation depends on how you want to structure the process table.

---

# 7. You can't simply let the scheduler decide when the quantum ends

This is the important part.

In xv6, scheduling is normally triggered by the **timer interrupt**.

So your MLFQ implementation needs the timer interrupt to track CPU ticks.

Conceptually:

```text
timer interrupt
      │
      ▼
current process
      │
      ├── ticks_used++
      ├── total_ticks++
      │
      └── quantum exhausted?
               │
              yes
               │
               ▼
           yield()
```

So in your timer-handling code:

```c
if (myproc() != 0) {

    struct proc *p = myproc();

    p->ticks_used++;
    p->total_ticks++;

    if (p->ticks_used >= quantum[p->queue_level]) {
        p->ticks_used = 0;

        if (p->queue_level < NQUEUE - 1)
            p->queue_level++;

        yield();
    }
}
```

**Don't copy this blindly into `trap.c` without checking your xv6 version**, because the exact timer-interrupt path can differ depending on the xv6 codebase/version and scheduler modifications.

---

# 8. Demotion

The fundamental MLFQ rule is usually:

> If a process uses its entire quantum, lower its priority.

For example:

```text
Process starts:
Q0, quantum = 1

uses 1 tick
    ↓
Q1, quantum = 2

uses 2 ticks
    ↓
Q2, quantum = 4

uses 4 ticks
    ↓
Q3, quantum = 8
```

Code:

```c
if (p->ticks_used >= quantum[p->queue_level]) {

    p->ticks_used = 0;

    if (p->queue_level < NQUEUE - 1)
        p->queue_level++;

    yield();
}
```

The boundary check prevents:

```text
Q3 → Q4
```

when Q3 is your lowest queue.

---

# 9. What if a process blocks before using its quantum?

Suppose:

```c
read(fd, buffer, n);
```

causes the process to block after only one tick.

It hasn't consumed its entire quantum.

A common MLFQ policy is:

```text
process uses partial quantum
       ↓
blocks
       ↓
keep same priority
```

This is useful because interactive/I/O-heavy processes tend to remain at higher priority.

So:

```text
CPU-heavy:
Q0 → Q1 → Q2 → Q3

I/O-heavy:
Q0 → Q0 → Q0 → Q0
```

assuming it repeatedly blocks before exhausting its quantum.

---

# 10. Aging

Without aging, a Q3 process could theoretically starve:

```text
Q0: A B C D E F ...
Q1: process X
Q2: process Y
Q3: process Z
```

If higher queues always have runnable processes, Z might never run.

So track:

```c
p->wait_ticks++;
```

for runnable processes that aren't selected.

Then:

```c
if (p->wait_ticks >= BOOST_INTERVAL) {

    if (p->queue_level > 0)
        p->queue_level--;

    p->wait_ticks = 0;
}
```

Conceptually:

```text
Q3 process waits too long
          ↓
       aging
          ↓
         Q2
          ↓
       waits again
          ↓
         Q1
          ↓
         Q0
```

You need to be careful about **when** you increment `wait_ticks`. You generally want it to represent time spent runnable but not running, not wall-clock time indiscriminately.

---

# 11. Global priority boost

Another common MLFQ policy is a periodic boost:

```text
Every 100 ticks:
    ALL RUNNABLE processes → Q0
```

This is simpler than per-process aging.

For example:

```c
if (ticks % BOOST_INTERVAL == 0) {

    for (each process p) {
        acquire(&p->lock);

        if (p->state == RUNNABLE ||
            p->state == RUNNING) {

            p->queue_level = 0;
            p->ticks_used = 0;
        }

        release(&p->lock);
    }
}
```

Then:

```text
Q3 ─┐
Q2 ─┤
Q1 ─┤── periodic boost ──→ Q0
Q0 ─┘
```

This is often easier to implement correctly than individual aging.

**Which one you should use depends on your assignment's MLFQ specification.** Don't add both unless required.

---

# 12. Be careful with locks

This is one of the trickiest parts of modifying xv6's scheduler.

`struct proc` has:

```c
struct spinlock lock;
```

and scheduler code typically needs to acquire the process lock before modifying:

```c
p->state
p->queue_level
p->ticks_used
```

For example:

```c
acquire(&p->lock);

if (p->state == RUNNABLE) {
    p->state = RUNNING;
    ...
}

release(&p->lock);
```

Don't scan the process table and modify MLFQ fields without considering the locking rules already used by xv6.

---

# 13. A more realistic rough scheduler

Putting the pieces together, the architecture could be:

```c
#define NQUEUE 4

static int quantum[NQUEUE] = {
    1, 2, 4, 8
};

void
scheduler(void)
{
    struct proc *p;
    struct cpu *c = mycpu();

    c->proc = 0;

    for (;;) {

        intr_on();

        /*
         * Search queues from highest priority
         * to lowest priority.
         */
        for (int q = 0; q < NQUEUE; q++) {

            struct proc *chosen = 0;

            /*
             * Find a RUNNABLE process in queue q.
             */
            for (p = proc;
                 p < &proc[NPROC];
                 p++) {

                acquire(&p->lock);

                if (p->state == RUNNABLE &&
                    p->queue_level == q) {

                    chosen = p;
                    break;
                }

                release(&p->lock);
            }

            if (chosen == 0)
                continue;

            /*
             * chosen->lock is already held.
             */
            chosen->state = RUNNING;
            c->proc = chosen;

            chosen->ticks_used = 0;
            chosen->wait_ticks = 0;

            swtch(&c->context,
                  &chosen->context);

            c->proc = 0;

            release(&chosen->lock);

            /*
             * Restart from Q0 after scheduling.
             */
            break;
        }
    }
}
```

The important thing here is:

```text
Q0
 ↓
find RUNNABLE process
 ↓
if found → run it
 ↓
return from swtch()
 ↓
restart at Q0

if none:
 ↓
Q1
 ↓
...
 ↓
Q3
```

---

# 14. What happens when `yield()` occurs?

Suppose Q0 has:

```text
quantum = 2
```

Process P runs:

```text
tick 1 → ticks_used = 1
tick 2 → ticks_used = 2
```

At tick 2:

```text
ticks_used >= quantum
```

so:

```c
p->queue_level++;
p->ticks_used = 0;
yield();
```

Now:

```text
Before:
P → Q0

After:
P → Q1
```

When `yield()` eventually returns to the scheduler:

```text
scheduler
   ↓
starts scanning at Q0
   ↓
Q0 processes?
   ↓
Q1
   ↓
find P
   ↓
run P
```

---

# 15. Don't forget process creation

If your shell runs:

```bash
fork()
```

new processes should receive appropriate initial scheduling state.

In `allocproc()`:

```c
p->queue_level = 0;
p->ticks_used = 0;
p->wait_ticks = 0;
p->total_ticks = 0;
```

But check your assignment's expected fork semantics. Some MLFQ implementations require a child to **inherit the parent's queue**, while others place every new process in Q0.

If unspecified, Q0 is a reasonable starting point, but follow the exact assignment specification if one exists.

---

# 16. How I'd organize the code

Don't put everything inside `scheduler()`.

I'd separate the logic:

```text
kernel/
├── proc.h
│     └── MLFQ fields
│
├── proc.c
│     ├── allocproc()
│     ├── scheduler()
│     ├── yield()
│     └── MLFQ helper functions
│
├── trap.c
│     └── timer tick accounting
│
└── param.h
      └── queue/quantum constants
```

For example:

```c
int
get_quantum(int level)
{
    static int q[] = {1, 2, 4, 8};

    return q[level];
}
```

and:

```c
void
demote(struct proc *p)
{
    if (p->queue_level < NQUEUE - 1)
        p->queue_level++;

    p->ticks_used = 0;
}
```

Then your scheduler isn't full of MLFQ arithmetic.

---

# 17. The complete MLFQ flow

Think about one process:

```text
                     fork()
                       │
                       ▼
                      Q0
                  quantum = 1
                       │
                  runs 1 tick
                       │
                       ▼
                      Q1
                  quantum = 2
                       │
             ┌─────────┴──────────┐
             │                    │
       uses entire quantum      blocks
             │                    │
             ▼                    ▼
            Q2                   Q1
         quantum=4           same level
             │
          4 ticks
             │
             ▼
            Q3
         quantum=8
```

Meanwhile:

```text
High priority
     │
     ▼
    Q0 ←──── new / boosted process
     │
     ▼
    Q1
     │
     ▼
    Q2
     │
     ▼
    Q3
     │
     ▼
Low priority
```

And the scheduler always asks:

```text
"Is there a runnable Q0 process?"
        │
       yes → run it
        │
       no
        ▼
"Is there a runnable Q1 process?"
        │
       yes → run it
        │
       no
        ▼
...
```

---

## The three pieces you should implement first

Don't try to implement aging, priority boosts, statistics, etc. all at once. Get these working first:

### 1. Add fields

```c
int queue_level;
int ticks_used;
```

### 2. Initialize them

```c
p->queue_level = 0;
p->ticks_used = 0;
```

### 3. Make timer ticks cause demotion

```c
p->ticks_used++;

if (p->ticks_used >= quantum[p->queue_level]) {

    p->ticks_used = 0;

    if (p->queue_level < NQUEUE - 1)
        p->queue_level++;

    yield();
}
```

### 4. Make `scheduler()` select the highest-priority runnable process

```text
for q = 0 → NQUEUE-1:
    find RUNNABLE process with queue_level == q
    if found:
        run it
        break
```

Once those work, add **aging/boosting** according to your assignment requirements.

One important xv6-specific point: **don't implement the quantum purely inside `scheduler()`** by measuring time before/after `swtch()`. The scheduler isn't running while the process is running. The **timer interrupt** is what gives you the opportunity to count ticks and force `yield()` when the quantum is exhausted.

## You

Implement ⁠ mlfq_tick() ⁠ in xv6 timer interrupt handler

## ChatGPT

Yes. The clean way is to make `mlfq_tick()` handle **one timer tick for the currently running process**, then call it from xv6's timer interrupt path.

Assuming your `struct proc` has:

```c
int queue_level;
int ticks_used;
```

and you have:

```c
#define NQUEUE 4

static int quantum[NQUEUE] = {1, 2, 4, 8};
```

### 1. Implement `mlfq_tick()`

In `kernel/proc.c`:

```c
void
mlfq_tick(void)
{
    struct proc *p = myproc();

    if (p == 0)
        return;

    acquire(&p->lock);

    // Only account CPU time for a currently running process.
    if (p->state == RUNNING) {
        p->ticks_used++;

        // Has the process exhausted its time quantum?
        if (p->ticks_used >= quantum[p->queue_level]) {

            // Reset quantum usage.
            p->ticks_used = 0;

            // Demote the process, unless it is already
            // in the lowest-priority queue.
            if (p->queue_level < NQUEUE - 1)
                p->queue_level++;

            release(&p->lock);

            // Give the scheduler a chance to select another process.
            yield();
            return;
        }
    }

    release(&p->lock);
}
```

However, **there is an important xv6 locking issue here**: `yield()` itself acquires `p->lock`, so you must **release the lock before calling `yield()`**, as above.

---

### 2. Add the declaration

In `kernel/defs.h`:

```c
void mlfq_tick(void);
```

---

### 3. Call it from the timer interrupt

The exact location depends on your xv6 version.

In standard xv6-riscv, timer interrupts eventually reach the timer handling code in `kernel/trap.c`. Find the timer interrupt section, typically around:

```c
if (which_dev == 2) {
    ...
}
```

or the timer handling code associated with `devintr()`.

You want the MLFQ accounting to happen on **every timer tick**:

```c
if (which_dev == 2) {
    mlfq_tick();
}
```

So conceptually:

```c
void
usertrap(void)
{
    ...

    if (which_dev == 2) {
        mlfq_tick();
    }

    ...
}
```

But **don't blindly put it in `usertrap()`** if your particular xv6 tree handles timer interrupts differently. The key is:

```text
Timer interrupt
      ↓
one timer tick
      ↓
mlfq_tick()
      ↓
current process used another tick
      ↓
quantum exhausted?
     / \
   no   yes
   │     │
   │     ├── reset ticks_used
   │     ├── demote queue
   │     └── yield()
   │
continue
```

### One correction to the earlier rough draft

I would actually recommend keeping the **tick counter and queue transition separate from the scheduler selection**:

```c
void
mlfq_tick(void)
{
    struct proc *p = myproc();

    if (p == 0)
        return;

    if (p->state != RUNNING)
        return;

    p->ticks_used++;

    if (p->ticks_used >= quantum[p->queue_level]) {
        p->ticks_used = 0;

        if (p->queue_level < NQUEUE - 1)
            p->queue_level++;

        yield();
    }
}
```

Whether you need `p->lock` here depends on **where your timer handler runs and your xv6 version's locking/interrupt context**. In particular, calling `acquire()` from an interrupt path can be problematic if the interrupted code already holds that process's lock. So before choosing the exact version, I'd inspect your `kernel/trap.c` and `kernel/proc.c`.

If you paste your current **`kernel/trap.c` timer-interrupt section and `struct proc`**, I can give you the exact `mlfq_tick()` implementation for your xv6 tree rather than a generic version.

## You

how to implement boost also tell me how to update makefile for SCHEDULER=MLFQ in xv6 while keeping original rr make qemu

## ChatGPT

Yes. For MLFQ, I would implement **periodic priority boosting** rather than per-process aging unless your assignment specifically asks for aging.

The idea is:

```text
Every BOOST_INTERVAL ticks:

Q0 ────────────────┐
Q1 ────────────────┤
Q2 ────────────────┼──→ Q0
Q3 ────────────────┘
```

This prevents starvation of CPU-bound processes.

## 1. Add a boost counter

You can keep a global tick counter for MLFQ.

In `kernel/proc.c`:

```c
#define BOOST_INTERVAL 100

static uint boost_ticks = 0;
```

Then your `mlfq_tick()` can look roughly like:

```c
void
mlfq_tick(void)
{
    struct proc *p = myproc();

    if(p == 0)
        return;

    p->ticks_used++;

    boost_ticks++;

    if(boost_ticks >= BOOST_INTERVAL) {
        boost_ticks = 0;
        mlfq_boost();
    }

    if(p->ticks_used >= quantum[p->queue_level]) {
        p->ticks_used = 0;

        if(p->queue_level < NQUEUE - 1)
            p->queue_level++;

        yield();
    }
}
```

But **don't actually use a global `boost_ticks` like this if `mlfq_tick()` is called once per CPU on an SMP xv6**. You can get multiple increments per timer period. Since you're currently running with `CPUS=1`, it will work for your setup, but a cleaner implementation is to use xv6's global `ticks` counter.

---

# 2. Better implementation using xv6's `ticks`

xv6 already maintains a global timer tick count.

So you can use:

```c
if(ticks % BOOST_INTERVAL == 0)
    mlfq_boost();
```

However, `ticks` is protected by `tickslock`, so don't casually access it from arbitrary code without considering the existing locking.

An even cleaner design is to have the timer interrupt detect the boost event and then invoke the boost logic.

For a basic assignment, you can define:

```c
#define BOOST_INTERVAL 100
```

and maintain:

```c
static uint mlfq_ticks;
```

with the assumption of `CPUS=1`.

---

# 3. Implement `mlfq_boost()`

In `kernel/proc.c`:

```c
void
mlfq_boost(void)
{
    struct proc *p;

    for(p = proc; p < &proc[NPROC]; p++) {

        acquire(&p->lock);

        if(p->state == RUNNABLE ||
           p->state == RUNNING) {

            p->queue_level = 0;
            p->ticks_used = 0;
        }

        release(&p->lock);
    }
}
```

Then declare it in `kernel/defs.h`:

```c
void mlfq_tick(void);
void mlfq_boost(void);
```

The important thing is that **sleeping processes don't necessarily need their queue changed** unless your assignment specifies that all processes should be boosted.

A common policy is:

```text
RUNNABLE → Q0
RUNNING  → Q0
SLEEPING → leave as-is
ZOMBIE   → ignore
UNUSED    → ignore
```

When the sleeping process wakes up, you can decide whether it should return to Q0 or retain its previous queue depending on your specification.

---

# 4. Quantum definitions

I'd put the constants somewhere centralized, e.g. `kernel/param.h`:

```c
#define NQUEUE 4
#define BOOST_INTERVAL 100
```

Then in `proc.c`:

```c
static int quantum[NQUEUE] = {
    1,
    2,
    4,
    8
};
```

So:

```text
Queue       Quantum
-------------------
Q0             1
Q1             2
Q2             4
Q3             8
```

You can adjust these according to your assignment.

---

# 5. One important issue with boosting

Suppose:

```text
Q0: A
Q1: B
Q2: C
Q3: D
```

At the boost:

```text
Q0: A B C D
Q1:
Q2:
Q3:
```

Now all processes have:

```c
queue_level = 0;
ticks_used = 0;
```

The next scheduler invocation starts from Q0.

This is exactly what you want.

---

# 6. Now the Makefile part

You want:

```bash
make qemu
```

to continue using the **original RR scheduler**, while:

```bash
make qemu SCHEDULER=MLFQ
```

uses MLFQ.

This is a good approach because you don't have to maintain separate Makefiles.

At the **top of your Makefile**, add:

```make
SCHEDULER ?= RR
```

This means:

```text
make qemu
        ↓
SCHEDULER = RR

make qemu SCHEDULER=MLFQ
        ↓
SCHEDULER = MLFQ
```

---

# 7. Pass the scheduler to the C compiler

You need to make the C code know which scheduler was selected.

Find the `CFLAGS` definition in your Makefile.

You can add:

```make
CFLAGS += -DSCHEDULER_$(SCHEDULER)
```

So:

```bash
make qemu
```

becomes approximately:

```text
-D SCHEDULER_RR
```

and:

```bash
make qemu SCHEDULER=MLFQ
```

becomes:

```text
-D SCHEDULER_MLFQ
```

---

# 8. Select the scheduler in `proc.c`

Then in `kernel/proc.c`:

```c
#ifdef SCHEDULER_MLFQ

void
scheduler(void)
{
    ...
}

#else

void
scheduler(void)
{
    // original xv6 RR scheduler
    ...
}

#endif
```

But I **wouldn't duplicate the entire scheduler function** if you can avoid it.

A cleaner approach is:

```c
void
scheduler(void)
{
#ifdef SCHEDULER_MLFQ
    mlfq_scheduler();
#else
    rr_scheduler();
#endif
}
```

Then:

```c
void
rr_scheduler(void)
{
    // original xv6 scheduler
}
```

and:

```c
void
mlfq_scheduler(void)
{
    // your MLFQ scheduler
}
```

Add declarations if needed:

```c
void rr_scheduler(void);
void mlfq_scheduler(void);
```

---

# 9. Even cleaner: choose inside the scheduler loop

You can also do:

```c
void
scheduler(void)
{
#ifdef SCHEDULER_MLFQ

    mlfq_scheduler();

#else

    rr_scheduler();

#endif
}
```

This gives you:

```text
                    scheduler()
                        │
              ┌─────────┴─────────┐
              │                   │
       SCHEDULER_MLFQ          SCHEDULER_RR
              │                   │
              ▼                   ▼
     mlfq_scheduler()      rr_scheduler()
```

---

# 10. Makefile example

Your Makefile will probably have something roughly like:

```make
CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer ...
```

Modify it to:

```make
SCHEDULER ?= RR

CFLAGS += -DSCHEDULER_$(SCHEDULER)
```

So the beginning might look like:

```make
SCHEDULER ?= RR

CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer \
         -ggdb -gdwarf-2

CFLAGS += -DSCHEDULER_$(SCHEDULER)
```

Then:

```bash
make clean
make qemu
```

uses:

```text
SCHEDULER_RR
```

while:

```bash
make clean
make qemu SCHEDULER=MLFQ
```

uses:

```text
SCHEDULER_MLFQ
```

---

# 11. One thing to check in your Makefile

Some xv6 assignment versions already have scheduler selection implemented.

For example, you might see something like:

```make
ifndef SCHEDULER
SCHEDULER := RR
endif

CFLAGS += -DSCHEDULER_$(SCHEDULER)
```

or:

```make
CFLAGS += -D$(SCHEDULER)
```

**If you already have this, don't add another definition.**

Run:

```bash
grep -n "SCHEDULER" Makefile
```

If you send me the output of that command, I can tell you exactly what to change.

---

## Recommended structure

For your implementation, I'd aim for:

```text
kernel/proc.h
    │
    ├── queue_level
    ├── ticks_used
    └── maybe wait_ticks

kernel/proc.c
    │
    ├── quantum[] = {1,2,4,8}
    ├── mlfq_tick()
    ├── mlfq_boost()
    ├── mlfq_scheduler()
    └── original rr_scheduler()

kernel/trap.c
    │
    └── timer interrupt
             │
             └── mlfq_tick()

kernel/defs.h
    │
    ├── mlfq_tick()
    └── mlfq_boost()

kernel/param.h
    │
    ├── NQUEUE
    └── BOOST_INTERVAL

Makefile
    │
    └── SCHEDULER ?= RR
             │
       ┌─────┴──────┐
       ▼            ▼
      RR           MLFQ
```

Then you can test both without changing the source:

```bash
# Original xv6 scheduler
make clean
make qemu

# MLFQ
make clean
make qemu SCHEDULER=MLFQ
```

Since you're using `CPUS=1` for your xv6 testing, I'd keep the first implementation single-CPU and get **queue selection → quantum exhaustion → demotion → boost** working before making the tick/boost accounting SMP-safe.