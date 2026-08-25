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
5. **Execution:** Can execute external system commands seamlessly.

## Getting Started

### Prerequisites
* A C compiler (e.g., `gcc`)
* `make`

### Building the Shell

The project comes with a `Makefile` for easy compilation.

```bash
cd c-shell
make clean
make all
```

This will produce the `shell.out` binary.

### Running the Shell

To start the shell, simply run the compiled binary:

```bash
./shell.out
```

## Running Tests

The project includes a comprehensive test suite for the lexer and grammar validator to ensure robust token parsing and strict adherence to the grammar rules.

To run the lexer tests:
```bash
gcc -std=c23 -Iinclude src/lexer.c tests/test_lexer.c -o test_lexer.out
./test_lexer.out
```

## Structure

* `src/` - Contains the source code (`main.c`, `lexer.c`, `exec.c`, `prompt.c`, etc.)
* `include/` - Contains the header files (`lexer.h`, `prompt.h`, etc.)
* `tests/` - Contains test files ensuring robustness of individual modules.
