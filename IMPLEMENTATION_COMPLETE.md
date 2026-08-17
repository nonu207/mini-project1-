# Shell Implementation Summary - A1 & A2 Complete

## What Works Now

### ✅ A1: Shell Prompt Display
- Shows format: `username@hostname:path$`
- Displays actual username (not UID)
- Path compression: `~` for home, `~/subdir` for subdirectories
- Full path for other locations
- Updates after directory changes

Example:
```
saanvijain07@Saanvis-MacBook-Air.local:~/projects/mini-project1/c-shell$
```

### ✅ A2: Tokenization & Parsing
- Input is split into tokens (command + arguments)
- Handles multiple spaces and tabs correctly
- Proper memory allocation for each token
- NULL-terminated token array for `exec()` compatibility

Example: `ls -la /home` becomes:
```
tokens[0] = "ls"
tokens[1] = "-la"
tokens[2] = "/home"
tokens[3] = NULL
```

### ✅ A3: Command Execution (Bonus - Implemented)
- **Built-in Commands:**
  - `exit` / `quit` - Exits the shell
  - `cd [path]` - Changes working directory (no path = home)
  - `pwd` - Prints current working directory

- **External Commands:**
  - Fork + execvp pattern for any command
  - Waits for child process to complete
  - Error handling with perror()

- **Examples that work:**
  ```bash
  $ ls -la                    # List files
  $ echo hello world          # Echo text
  $ cd /tmp                   # Change directory
  $ pwd                       # Show path
  $ ls -la /etc               # List with arguments
  $ cat file.txt              # Read files
  $ exit                      # Exit shell
  ```

---

## Architecture

### Data Flow
```
User Input (stdin)
    ↓
fgets() → char input[1024]
    ↓
tokenize(input, &tokens) → TokenList
    ↓
execute_command(&tokens)
    ├→ Check: exit/quit? → exit(0)
    ├→ Check: cd? → chdir()
    ├→ Check: pwd? → getcwd() + printf
    └→ External cmd? → fork() → execvp()
    ↓
free_tokens(&tokens) → cleanup
    ↓
Loop back to prompt
```

### File Structure
```
src/
├── main.c          - REPL loop + execute_command()
├── lexer.c         - tokenize() + free_tokens()
└── prompt.c        - display_prompt() + init_prompt()

include/
├── lexer.h         - TokenList typedef + function declarations
└── prompt.h        - prompt functions
```

---

## How the Tokenizer Works

### Step-by-Step: `echo hello world`

```
Input string: "echo hello world"
                    ↓
tokenize() scans character by character:
1. 'e','c','h','o' + space → Token 1: allocate + copy "echo"
2. 'h','e','l','l','o' + space → Token 2: allocate + copy "hello"
3. 'w','o','r','l','d' + EOF → Token 3: allocate + copy "world"
                    ↓
Result structure:
{
    tokens[0] → malloc(5) = "echo"
    tokens[1] → malloc(6) = "hello"
    tokens[2] → malloc(6) = "world"
    tokens[3] → NULL (terminator)
    count = 3
}
                    ↓
Pass to execute_command():
    fork() in parent
    execvp("echo", {"echo", "hello", "world", NULL}) in child
    Output: "hello world"
```

---

## How to Extend This

### Add New Built-in Commands

In `src/main.c`, add to `execute_command()`:

```c
// Example: Add "help" command
if (strcmp(cmd, "help") == 0) {
    printf("Available commands:\n");
    printf("  exit/quit - Exit shell\n");
    printf("  cd [path] - Change directory\n");
    printf("  pwd - Print working directory\n");
    printf("  help - Show this help\n");
    return;
}
```

### Token Scanning Example

You can inspect tokens before executing:

```c
// In execute_command(), before dispatch:
printf("Command: %s\n", tokens->tokens[0]);
printf("Args: ");
for (int i = 1; i < tokens->count; i++) {
    printf("%s ", tokens->tokens[i]);
}
printf("\n");
```

### Error Checking

Current implementation handles:
- ✅ Empty input (skipped)
- ✅ tokenize failures (error message)
- ✅ fork failures (perror)
- ✅ execvp failures (perror + exit code 127)
- ✅ cd failures (perror)

---

## Compilation & Testing

### Build
```bash
cd /Users/saanvijain07/projects/mini-project1/c-shell
make clean      # Remove old binaries
make all        # Compile shell.out
```

### Run
```bash
./shell.out
```

### Test Commands
```bash
$ pwd                          # Should show: /path/to/c-shell
$ ls -la                       # Should list files
$ cd ..                        # Should change to parent
$ pwd                          # Should show: /path/to/mini-project1
$ echo "Hello, Shell!"         # Should print: Hello, Shell!
$ exit                         # Should exit
```

---

## Code Quality

### Compilation Flags
```bash
-std=c23                        # C23 standard
-D_POSIX_C_SOURCE=200809L       # POSIX features
-D_XOPEN_SOURCE=700             # X/Open features
-D_DARWIN_C_SOURCE              # macOS features
-Wall -Wextra -Werror           # Strict warnings
-Wno-unused-parameter           # Allow unused params
-fno-asm                        # No inline assembly
```

### Result: ✅ Zero warnings, zero errors

---

## Memory Management

### Safe Patterns Used

1. **Tokenize**: Allocate `malloc()` for each token
2. **Use**: Access `tokens.tokens[i]` and `tokens.count`
3. **Cleanup**: Call `free_tokens()` to deallocate

Example:
```c
TokenList tokens;
tokenize(input, &tokens);       // Allocate
execute_command(&tokens);       // Use
free_tokens(&tokens);           // Free

// CRITICAL: Never use tokens after free_tokens()!
```

---

## What's Next (Optional Enhancements)

If you want to extend this further:

1. **Signal Handling**: Catch SIGINT (Ctrl+C) to prevent shell exit
2. **Job Control**: Background processes (&), fg, bg, jobs
3. **Pipes**: Chain commands with |
4. **Redirection**: Input <, output >, append >>
5. **Environment Variables**: $HOME, $PATH, $PWD
6. **History**: Store recent commands (arrow keys)
7. **Tab Completion**: Auto-complete file names

---

## Testing Checklist

- [x] Prompt displays correctly with user@host:path format
- [x] Tokenizer splits "ls -la /home" into 3 tokens
- [x] `exit` command exits the shell
- [x] `cd` command changes directory
- [x] `pwd` command shows correct path
- [x] `echo hello` runs external command correctly
- [x] Path compression works (~ for home directory)
- [x] No memory leaks (free_tokens called properly)
- [x] Compilation with zero warnings

---

## Key Learning Points

1. **Tokenization**: Manual parsing better than `strtok()` for memory safety
2. **Process Management**: `fork()` for child process, `execvp()` for execution
3. **Memory Safety**: Always `free()` what you `malloc()`
4. **Error Handling**: Use `perror()` for system call failures
5. **Shell Loop**: Read → Parse → Execute → Clean → Repeat
6. **NULL Termination**: Essential for `exec*()` functions

---

This completes the basic shell implementation! The architecture is clean, extensible, and ready for more features.
