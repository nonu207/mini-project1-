#include "shell.h"
#include <fcntl.h>
#include <sys/types.h>

#define CHUNK_SIZE 4096

/* Reports whether a line consists entirely of whitespace, including its
 * trailing newline. */
static int is_empty_line(const char *line) {
    if (line == NULL) return 1;
    for (int i = 0; line[i] != '\0'; i++) {
        if (!isspace((unsigned char)line[i])) return 0;
    }
    return 1;
}

/* Reads a file from start to finish and prints it, numbering non-empty
 * lines when flag_n is set. line_num is a running counter that
 * persists across multiple files, so numbering continues sequentially
 * from one file into the next. */
static void peek_forward(FILE *f, int flag_n, int *line_num) {
    char   *line = NULL;
    size_t  len  = 0;

    while (getline(&line, &len, f) != -1) {
        size_t line_len = strlen(line);
        int    has_nl   = (line_len > 0 && line[line_len - 1] == '\n');

        if (flag_n) {
            if (!is_empty_line(line))
                printf("%d %s", (*line_num)++, line);
            else
                printf("%s", line);
        } else {
            printf("%s", line);
        }

        if (!has_nl)
            putchar('\n');
    }
    free(line);
}

/* Reads every line of a file into memory, assigning each one its
 * original forward line number so that number survives being printed
 * in reverse, then prints the lines back to front.
 *
 * This is used only for non-seekable input with the reverse flag set,
 * such as stdin, a pipe, or a FIFO. A regular file is instead handled
 * by peek_reverse_seekable, which never needs to buffer the whole
 * file. */
static void peek_reverse_buffered(FILE *f, int flag_n, int *line_num) {
    char **lines  = NULL;
    int   *nums   = NULL;
    int    count  = 0;
    int    cap    = 64;

    lines = malloc(cap * sizeof(char *));
    nums  = malloc(cap * sizeof(int));
    if (!lines || !nums) {
        free(lines);
        free(nums);
        return;
    }

    char   *line = NULL;
    size_t  len  = 0;

    /* Forward pass: lines are read and given numbers in their original
     * order. */
    while (getline(&line, &len, f) != -1) {
        if (count >= cap) {
            cap   *= 2;
            lines  = realloc(lines, cap * sizeof(char *));
            nums   = realloc(nums,  cap * sizeof(int));
        }
        lines[count] = strdup(line);
        if (flag_n && !is_empty_line(line))
            nums[count] = (*line_num)++;
        else
            nums[count] = 0;
        count++;
    }
    free(line);

    /* Reverse pass: the lines are printed back to front, each with the
     * number it was given in the forward pass. */
    for (int i = count - 1; i >= 0; i--) {
        const char *text    = lines[i];
        size_t      text_len = strlen(text);
        int         has_nl   = (text_len > 0 && text[text_len - 1] == '\n');

        if (flag_n && nums[i] != 0)
            printf("%d %s", nums[i], text);
        else
            printf("%s", text);

        /* If the stored line has no trailing newline, which happens
         * for the last line of a file with no newline at the end, one
         * is added so lines do not run together. */
        if (!has_nl)
            putchar('\n');

        free(lines[i]);
    }
    free(lines);
    free(nums);
}

/* Makes a forward pass over a file descriptor in fixed-size chunks,
 * counting how many lines contain at least one non-whitespace
 * character. Nothing is stored, so memory use stays constant no
 * matter how large the file is. */
static int count_nonempty_lines(int fd) {
    char    buf[CHUNK_SIZE];
    int     count       = 0;
    int     has_content = 0;
    ssize_t n;

    lseek(fd, 0, SEEK_SET);
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t k = 0; k < n; k++) {
            if (buf[k] == '\n') {
                count += has_content;
                has_content = 0;
            } else if (!isspace((unsigned char)buf[k])) {
                has_content = 1;
            }
        }
    }
    return count + has_content;
}

/* Prints one line during a reverse read, without adding another
 * newline beyond the one already appended. Line numbers count down
 * rather than up, since lines are being emitted from the end of the
 * file toward the beginning. */
static void emit_reverse_line(const char *text, int flag_n, int *next_num) {
    if (flag_n && !is_empty_line(text))
        printf("%d %s\n", (*next_num)--, text);
    else
        printf("%s\n", text);
}

/* Reads a regular file backwards in fixed-size chunks using lseek, so
 * the whole file never needs to be held in memory.
 *
 * When line numbers are requested, a forward counting pass first finds
 * how many numbered lines the file has, so the last line can be given
 * its correct original number right away, and the numbers can simply
 * be counted down from there as the reverse pass proceeds. */
static void peek_reverse_seekable(int fd, int flag_n, int *line_num) {
    struct stat st;
    if (fstat(fd, &st) != 0) return;

    off_t filesize = st.st_size;
    if (filesize == 0) return;

    int total    = flag_n ? count_nonempty_lines(fd) : 0;
    int next_num = *line_num + total - 1;
    *line_num   += total;

    char   buf[CHUNK_SIZE];
    off_t  pos      = filesize;
    char  *leftover = NULL;

    while (pos > 0) {
        off_t   to_read   = (pos > CHUNK_SIZE) ? CHUNK_SIZE : pos;
        pos              -= to_read;

        lseek(fd, pos, SEEK_SET);
        ssize_t bytes_read = read(fd, buf, to_read);
        if (bytes_read <= 0) break;

        int i = (int)bytes_read - 1;

        /* The file's own trailing newline is skipped on the very
         * first chunk read, so it does not produce a spurious empty
         * line. */
        if (pos + to_read == filesize && i >= 0 && buf[i] == '\n')
            i--;

        /* The chunk is walked backwards, extracting one line at a
         * time. */
        while (i >= 0) {
            int end_idx = i;

            while (i >= 0 && buf[i] != '\n')
                i--;

            /* The piece of text found is buf[i+1 .. end_idx], which
             * may be empty for a blank line. */
            int   piece_len = end_idx - i;
            char *piece     = malloc(piece_len + 1);
            memcpy(piece, buf + i + 1, piece_len);
            piece[piece_len] = '\0';

            /* Any leftover text carried over from the previous chunk
             * is appended to the right of this piece, since it
             * belongs to the same line. */
            char *full_line;
            if (leftover != NULL) {
                full_line = malloc(piece_len + strlen(leftover) + 1);
                strcpy(full_line, piece);
                strcat(full_line, leftover);
                free(piece);
                free(leftover);
                leftover = NULL;
            } else {
                full_line = piece;
            }

            if (i >= 0) {
                /* A newline was found, so full_line is a complete
                 * line. */
                emit_reverse_line(full_line, flag_n, &next_num);
                free(full_line);
                i--;
            } else {
                /* The start of the chunk was reached with no newline
                 * found yet, so this text is saved to be completed by
                 * the next chunk read. */
                leftover = full_line;
            }
        }
    }

    /* Any text left over from the very beginning of the file is
     * flushed here. If nothing is left over, the file began with a
     * newline, so its first line is empty and still needs to be
     * printed. */
    if (leftover != NULL) {
        emit_reverse_line(leftover, flag_n, &next_num);
        free(leftover);
    } else if (pos == 0) {
        emit_reverse_line("", flag_n, &next_num);
    }
}

/* Dispatches one filename argument to the appropriate reader, handling
 * stdin, missing files, directories, and the choice between the
 * seekable and buffered reverse readers. */
static void process_file(const char *filename, int flag_n, int flag_r,
                         int *line_num) {
    if (filename == NULL || strcmp(filename, "-") == 0) {
        if (flag_r)
            peek_reverse_buffered(stdin, flag_n, line_num);
        else
            peek_forward(stdin, flag_n, line_num);
        /* The EOF condition on stdin is cleared so the shell's own
         * input loop can keep reading afterward. */
        clearerr(stdin);
        return;
    }

    struct stat st;
    if (stat(filename, &st) != 0) {
        fprintf(stderr, "peek: no such file or directory\n");
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        fprintf(stderr, "peek: is a directory\n");
        return;
    }

    if (flag_r) {
        if (S_ISREG(st.st_mode)) {
            int fd = open(filename, O_RDONLY);
            if (fd >= 0) {
                peek_reverse_seekable(fd, flag_n, line_num);
                close(fd);
            }
        } else {
            /* A non-seekable source, such as a FIFO, is read with the
             * buffered reverse reader instead. */
            FILE *f = fopen(filename, "r");
            if (f != NULL) {
                peek_reverse_buffered(f, flag_n, line_num);
                fclose(f);
            }
        }
    } else {
        FILE *f = fopen(filename, "r");
        if (f != NULL) {
            peek_forward(f, flag_n, line_num);
            fclose(f);
        }
    }
}

/* Implements the peek command. Parses the n and r flags, then either
 * reads stdin if no filenames were given, or validates every filename
 * first and only prints anything if all of them are valid. A single
 * line counter is shared across every file, so numbering continues
 * sequentially from one file into the next. */
void peek(int argc, char **argv) {
    int flag_n = 0;
    int flag_r = 0;
    int i      = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        for (int j = 1; argv[i][j] != '\0'; j++) {
            if      (argv[i][j] == 'n') flag_n = 1;
            else if (argv[i][j] == 'r') flag_r = 1;
            else {
                fprintf(stderr, "peek: invalid syntax\n");
                return;
            }
        }
        i++;
    }

    int line_num = 1;

    if (i == argc) {
        process_file(NULL, flag_n, flag_r, &line_num);
        return;
    }

    int all_valid = 1;
    for (int j = i; j < argc; j++) {
        if (strcmp(argv[j], "-") == 0) continue;

        struct stat st;
        if (stat(argv[j], &st) != 0) {
            fprintf(stderr, "peek: no such file or directory\n");
            all_valid = 0;
        } else if (S_ISDIR(st.st_mode)) {
            fprintf(stderr, "peek: is a directory\n");
            all_valid = 0;
        }
    }

    if (all_valid) {
        for (; i < argc; i++)
            process_file(argv[i], flag_n, flag_r, &line_num);
    }
}
