# Token Scanning Quick Reference

## The TokenList Structure

```c
TokenList {
    char *tokens[100];  // Array of pointers to strings
    int count;          // Number of tokens (0 to 100)
}
```

Example after parsing `cat file.txt`:
```
tokens.count = 2
tokens.tokens[0] = "cat"      (allocated string)
tokens.tokens[1] = "file.txt" (allocated string)
tokens.tokens[2] = NULL       (terminator)
```

---

## Scanning Patterns

### Pattern 1: Loop Through All Tokens
```c
for (int i = 0; i < tokens->count; i++) {
    printf("Token %d: %s\n", i, tokens->tokens[i]);
}
```

### Pattern 2: Process Command Separately from Args
```c
const char *cmd = tokens->tokens[0];  // Get command

// Process arguments
for (int i = 1; i < tokens->count; i++) {
    printf("Arg: %s\n", tokens->tokens[i]);
}
```

### Pattern 3: Access Specific Arguments
```c
if (tokens->count > 1) {
    printf("First arg: %s\n", tokens->tokens[1]);
}

if (tokens->count > 2) {
    printf("Second arg: %s\n", tokens->tokens[2]);
}
```

### Pattern 4: Check if Argument Exists
```c
// Safe access to tokens.tokens[1]
const char *arg1 = (tokens->count > 1) ? tokens->tokens[1] : NULL;

if (arg1 != NULL) {
    printf("User provided: %s\n", arg1);
}
```

### Pattern 5: Find a Specific Token
```c
int found = -1;
for (int i = 0; i < tokens->count; i++) {
    if (strcmp(tokens->tokens[i], "-v") == 0) {
        found = i;
        break;
    }
}

if (found != -1) {
    printf("Found -v flag at position %d\n", found);
}
```

### Pattern 6: Count Arguments (Exclude Command)
```c
int arg_count = tokens->count - 1;  // Subtract 1 for command itself
printf("Command: %s\n", tokens->tokens[0]);
printf("Number of arguments: %d\n", arg_count);
```

---

## Real-World Examples

### Example 1: Parsing `cd /tmp`
```c
// After tokenizing:
tokens.count = 2
tokens.tokens[0] = "cd"
tokens.tokens[1] = "/tmp"

// Get the path
const char *path = (tokens->count > 1) ? tokens->tokens[1] : getenv("HOME");
chdir(path);
```

### Example 2: Parsing `ls -la -h /etc`
```c
// After tokenizing:
tokens.count = 4
tokens.tokens[0] = "ls"
tokens.tokens[1] = "-la"
tokens.tokens[2] = "-h"
tokens.tokens[3] = "/etc"

// Scan for options and path
int has_la = 0;
int has_h = 0;
const char *path = ".";

for (int i = 1; i < tokens->count; i++) {
    if (strcmp(tokens->tokens[i], "-la") == 0) {
        has_la = 1;
    } else if (strcmp(tokens->tokens[i], "-h") == 0) {
        has_h = 1;
    } else if (tokens->tokens[i][0] != '-') {
        // Not a flag, must be path
        path = tokens->tokens[i];
    }
}

printf("ls with -la: %s, with -h: %s, path: %s\n", 
       has_la ? "yes" : "no", 
       has_h ? "yes" : "no", 
       path);
```

### Example 3: Parsing `grep pattern file.txt`
```c
// After tokenizing:
tokens.count = 3
tokens.tokens[0] = "grep"
tokens.tokens[1] = "pattern"
tokens.tokens[2] = "file.txt"

// Extract components
const char *pattern = (tokens->count > 1) ? tokens->tokens[1] : "";
const char *filename = (tokens->count > 2) ? tokens->tokens[2] : "";

// Or just pass to execvp:
execvp(tokens->tokens[0], tokens->tokens);
// Already NULL-terminated!
```

---

## Memory and Cleanup

### Safe Scanning Pattern
```c
// Scan tokens (before cleanup)
for (int i = 0; i < tokens->count; i++) {
    printf("%s ", tokens->tokens[i]);
}
printf("\n");

// After scanning, cleanup
free_tokens(&tokens);

// ❌ DO NOT access tokens->tokens[i] after free_tokens()!
```

### Inside execute_command()
```c
void execute_command(TokenList *tokens) {
    if (tokens == NULL || tokens->count == 0) {
        return;
    }

    // ✅ SAFE: tokens are allocated
    const char *cmd = tokens->tokens[0];
    
    // ✅ SAFE: scan through tokens
    for (int i = 1; i < tokens->count; i++) {
        if (strcmp(tokens->tokens[i], "-v") == 0) {
            // found verbose flag
        }
    }

    // Execute...
    fork();
    execvp(tokens->tokens[0], tokens->tokens);
    // tokens still valid here in child process
}

// ❌ After execute_command returns:
// tokens are still valid (not freed)
// Must call free_tokens() in main()!
```

---

## Common Mistakes

### ❌ Wrong: Accessing past count
```c
// tokens.count = 2
for (int i = 0; i <= tokens->count; i++) {  // Should be <, not <=
    printf("%s\n", tokens->tokens[i]);  // Accesses uninitialized tokens[2]
}
```

### ❌ Wrong: Accessing after free
```c
free_tokens(&tokens);
printf("%s\n", tokens->tokens[0]);  // CRASH! Freed memory
```

### ❌ Wrong: Not checking bounds
```c
// What if user only typed "cd" with no path?
printf("%s\n", tokens->tokens[1]);  // Crash if tokens.count == 1
```

### ✅ Right: Safe bounds checking
```c
if (tokens->count > 1) {
    printf("%s\n", tokens->tokens[1]);  // Safe
}
```

### ✅ Right: Using ternary operator
```c
const char *path = (tokens->count > 1) ? tokens->tokens[1] : "/home";
```

---

## Quick Checklist

- [ ] Always check `tokens->count > i` before accessing `tokens->tokens[i]`
- [ ] Remember: command is at index 0, arguments start at index 1
- [ ] Remember: `tokens->tokens[count]` is NULL (for exec compatibility)
- [ ] Always call `free_tokens()` after using tokens
- [ ] Never use tokens after calling `free_tokens()`
- [ ] Use `strcmp()` to compare token strings
- [ ] Use `tokens->count - 1` to get number of arguments

---

This completes the tokenizer documentation!
