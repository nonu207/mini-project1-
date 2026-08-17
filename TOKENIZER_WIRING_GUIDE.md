# How to Wire Input → Tokenizer → Scanner → Executor

## Overview of the Data Flow

```
User Input
    ↓
fgets() reads input into char[] buffer
    ↓
tokenize() parses into TokenList
    ↓
Loop through tokens.tokens[] array
    ↓
Process/execute based on command
    ↓
free_tokens() cleanup
```

---

## Step 1: Understanding the TokenList Structure

```c
typedef struct {
    char *tokens[MAX_TOKENS];  // Array of pointers to strings (up to 100 commands)
    int count;                  // How many tokens were parsed
} TokenList;
```

**Example after parsing "ls -la /home":**
```
tokens.count = 3
tokens.tokens[0] = "ls"      (pointer to allocated string)
tokens.tokens[1] = "-la"     (pointer to allocated string)
tokens.tokens[2] = "/home"   (pointer to allocated string)
tokens.tokens[3] = NULL      (NULL-terminated for exec)
```

---

## Step 2: Current Wiring in main.c

### The Input Loop Flow

```c
while (1) {
    display_prompt();                           // Show prompt

    // STEP 1: Get input
    if (fgets(input, sizeof(input), stdin) == NULL) {
        printf("\n");
        break;
    }

    // STEP 2: Clean up input
    input[strcspn(input, "\n")] = '\0';        // Remove newline
    
    if (strlen(input) == 0) {
        continue;                               // Skip empty lines
    }

    // STEP 3: TOKENIZE - Convert string to array of tokens
    int token_count = tokenize(input, &tokens);
    
    if (token_count < 0) {
        fprintf(stderr, "Error: Failed to tokenize input\n");
        continue;
    }

    if (token_count == 0) {
        continue;
    }

    // STEP 4: SCAN - Loop through tokens
    for (int i = 0; i < token_count; i++) {
        printf("  Token[%d]: %s\n", i, tokens.tokens[i]);
    }

    // STEP 5: Execute - Process the command (to be implemented)
    // ... add your command execution logic here ...

    // STEP 6: Cleanup - Free allocated memory
    free_tokens(&tokens);
}
```

---

## Step 3: How to Scan and Use Tokens

### Simple Approach: Check the Command

```c
// After tokenizing, you have:
// tokens.tokens[0] = command name
// tokens.tokens[1..n] = arguments

// Example: Check what command was entered
printf("Command is: %s\n", tokens.tokens[0]);
printf("Number of arguments: %d\n", token_count - 1);

// Access specific arguments
if (token_count > 1) {
    printf("First argument: %s\n", tokens.tokens[1]);
}
if (token_count > 2) {
    printf("Second argument: %s\n", tokens.tokens[2]);
}
```

### Better Approach: Use a Loop to Scan All Tokens

```c
// Scan through all tokens
for (int i = 0; i < token_count; i++) {
    printf("Arg %d: %s\n", i, tokens.tokens[i]);
}

// Or only scan arguments (skip the command at [0])
printf("Command: %s\n", tokens.tokens[0]);
printf("Arguments:\n");
for (int i = 1; i < token_count; i++) {
    printf("  %s\n", tokens.tokens[i]);
}
```

### Advanced Approach: Search for Specific Tokens

```c
// Find if a specific argument exists
int has_verbose = 0;
for (int i = 1; i < token_count; i++) {
    if (strcmp(tokens.tokens[i], "-v") == 0 || 
        strcmp(tokens.tokens[i], "--verbose") == 0) {
        has_verbose = 1;
        break;
    }
}

// Find index of a token
int equals_idx = -1;
for (int i = 0; i < token_count; i++) {
    if (strcmp(tokens.tokens[i], "=") == 0) {
        equals_idx = i;
        break;
    }
}
```

---

## Step 4: Implementing Command Execution

### Example: Handle Built-in Commands

Replace the debug printf section with:

```c
// STEP 4: Execute command based on type
const char *command = tokens.tokens[0];

// Handle built-in commands
if (strcmp(command, "exit") == 0 || strcmp(command, "quit") == 0) {
    printf("Exiting shell...\n");
    free_tokens(&tokens);
    break;  // Exit the main loop
}
else if (strcmp(command, "cd") == 0) {
    // Change directory command
    if (token_count < 2) {
        // cd with no arguments - go to home
        char *home = getenv("HOME");
        if (home && chdir(home) == 0) {
            printf("Changed to home directory\n");
        }
    } else {
        // cd with path argument
        if (chdir(tokens.tokens[1]) == 0) {
            printf("Changed directory to %s\n", tokens.tokens[1]);
        } else {
            perror("cd");
        }
    }
}
else if (strcmp(command, "pwd") == 0) {
    // Print working directory
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s\n", cwd);
    } else {
        perror("pwd");
    }
}
else {
    // External command - fork and exec
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
    } else if (pid == 0) {
        // Child process - execute the command
        // tokens.tokens is NULL-terminated, ready for execvp
        execvp(tokens.tokens[0], tokens.tokens);
        
        // If execvp returns, it failed
        perror(tokens.tokens[0]);
        exit(127);  // Standard exit code for command not found
    } else {
        // Parent process - wait for child to complete
        int status;
        waitpid(pid, &status, 0);
    }
}
```

---

## Step 5: Complete Example Implementation

Here's a more complete version you can integrate:

```c
#include "prompt.h"
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>

void execute_command(TokenList *tokens) {
    if (tokens == NULL || tokens->count == 0) {
        return;
    }

    const char *cmd = tokens->tokens[0];

    // Built-in: exit
    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        exit(0);
    }

    // Built-in: cd
    if (strcmp(cmd, "cd") == 0) {
        const char *path = (tokens->count > 1) ? tokens->tokens[1] : getenv("HOME");
        if (chdir(path) != 0) {
            perror("cd");
        }
        return;
    }

    // Built-in: pwd
    if (strcmp(cmd, "pwd") == 0) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
        } else {
            perror("pwd");
        }
        return;
    }

    // External command - fork and exec
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        // Child process
        execvp(tokens->tokens[0], tokens->tokens);
        perror(tokens->tokens[0]);
        exit(127);
    } else {
        // Parent process - wait for child
        int status;
        waitpid(pid, &status, 0);
    }
}

int main(void) {
    char input[1024];
    TokenList tokens;

    init_prompt();

    while (1) {
        display_prompt();

        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("\n");
            break;
        }

        input[strcspn(input, "\n")] = '\0';

        if (strlen(input) == 0) {
            continue;
        }

        // Tokenize the input
        int token_count = tokenize(input, &tokens);
        
        if (token_count <= 0) {
            continue;
        }

        // Execute the command
        execute_command(&tokens);

        // Clean up
        free_tokens(&tokens);
    }

    return 0;
}
```

---

## Step 6: Testing Your Implementation

### Test 1: Built-in Commands
```bash
$ ./shell.out
user@host:~$ pwd
/Users/saanvijain07/projects/mini-project1/c-shell
user@host:~$ cd /tmp
user@host:/tmp$ pwd
/tmp
user@host:/tmp$ exit
```

### Test 2: External Commands
```bash
$ ./shell.out
user@host:~$ ls -la
(listing of current directory)
user@host:~$ echo hello world
hello world
user@host:~$ date
(current date and time)
```

### Test 3: Token Scanning
Add this after tokenizing to debug:
```c
printf("Parsed %d tokens:\n", tokens.count);
for (int i = 0; i < tokens.count; i++) {
    printf("  [%d] = '%s'\n", i, tokens.tokens[i]);
}
```

---

## Detailed Token Structure Visualization

### Example: `ls -la /home`

```
input = "ls -la /home"
                ↓
        tokenize() function
                ↓
TokenList structure created:
┌─────────────────────────────────────────┐
│ TokenList {                             │
│   tokens[0] → "ls"     (allocated)      │
│   tokens[1] → "-la"    (allocated)      │
│   tokens[2] → "/home"  (allocated)      │
│   tokens[3] → NULL     (terminator)     │
│   count = 3                             │
│ }                                       │
└─────────────────────────────────────────┘
                ↓
        Ready for execvp()
                ↓
        execvp(tokens[0], tokens)
        → Runs: /bin/ls -la /home
```

---

## Memory Management: Critical!

### Proper Sequence:
```c
// 1. Tokenize (allocates memory)
tokenize(input, &tokens);

// 2. Use tokens
execute_command(&tokens);

// 3. Free (deallocates memory)
free_tokens(&tokens);

// Never use tokens after free_tokens()!
```

### What Gets Allocated:
```c
// Inside tokenize(), for each token:
malloc(token_length + 1)  // Allocate space for token string
strcpy(...)               // Copy the token

// Inside free_tokens(), for each token:
free(tokens->tokens[i])   // Free the string
tokens->tokens[i] = NULL  // Safety: null out pointer
```

---

## Key Points to Remember

1. **Tokenize Once Per Input**: Call `tokenize()` once after reading input
2. **NULL-Terminated Array**: `tokens.tokens[count]` is always NULL for `exec*` functions
3. **Memory Cleanup**: Always call `free_tokens()` before next iteration
4. **Scan Pattern**: Use `tokens.tokens[i]` to access each token, loop from `0` to `count-1`
5. **Command is Always First**: `tokens.tokens[0]` is always the command name
6. **Arguments Start at Index 1**: Arguments are `tokens.tokens[1]` through `tokens.tokens[count-1]`

---

## Common Mistakes to Avoid

### ❌ Wrong: Using tokens after freeing
```c
free_tokens(&tokens);
printf("%s", tokens.tokens[0]);  // CRASH! Accessing freed memory
```

### ❌ Wrong: Not NULL-terminating for exec
```c
// If tokens.tokens[count] is not NULL, exec will crash
char *argv[100];
argv[0] = "ls";
argv[1] = "-la";
// Missing: argv[2] = NULL;
execvp(argv[0], argv);  // CRASH!
```

### ❌ Wrong: Modifying original input string
```c
tokenize(input, &tokens);  // Don't use input after this!
input[0] = 'X';            // This might corrupt tokens!
```

### ✅ Right: Proper usage
```c
char input[1024];
TokenList tokens;

fgets(input, sizeof(input), stdin);
input[strcspn(input, "\n")] = '\0';

tokenize(input, &tokens);     // Tokenize
execute_command(&tokens);      // Use tokens
free_tokens(&tokens);          // Clean up

// Now input can be reused in next iteration
```

---

This gives you the complete picture of how to wire, scan, and execute commands!
