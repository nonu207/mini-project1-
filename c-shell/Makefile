# Compiler settings per assignment specification
CC = gcc
CFLAGS = -std=c23 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -fno-asm -Iinclude

# Directories
SRC_DIR = src
INC_DIR = include

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.c)

# Target output binary
TARGET = shell.out

# Default target
all: $(TARGET)

# Build rule
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

# Clean rule
clean:
	rm -f $(TARGET)

.PHONY: all clean