#include "shell.h"
#include <fcntl.h>
#include <sys/types.h>

#define CHUNK_SIZE 4096

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Returns 1 if line consists entirely of whitespace (including '\n'). */
static int is_empty_line(const char *line) {
    if (line == NULL) return 1;
    for (int i = 0; line[i] != '\0'; i++) {
        if (!isspace((unsigned char)line[i])) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* peek_forward: reads f from start to finish.                         */
/*   line_num — pointer to running counter (persists across files).    */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* peek_reverse_buffered: reads all lines from f into memory,          */
/* assigns ORIGINAL forward line numbers (so they are preserved when   */
/* printed in reverse), then prints in reverse order.                  */
/*                                                                      */
/* Used only for non-seekable input with -r (stdin, pipes, FIFOs).     */
/* Regular files go through peek_reverse_seekable instead.             */
/* ------------------------------------------------------------------ */
static void peek_reverse_buffered(FILE *f, int flag_n, int *line_num) {
    /* Dynamic array of lines */
    char **lines  = NULL;
    int   *nums   = NULL; /* line number assigned to each line (0 = no number) */
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

    /* Forward pass: read and assign numbers in original order */
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
            nums[count] = 0; /* empty line, no number */
        count++;
    }
    free(line);

    /* Reverse pass: print in reverse order with original numbers */
    for (int i = count - 1; i >= 0; i--) {
        const char *text    = lines[i];
        size_t      text_len = strlen(text);
        int         has_nl   = (text_len > 0 && text[text_len - 1] == '\n');

        if (flag_n && nums[i] != 0)
            printf("%d %s", nums[i], text);
        else
            printf("%s", text);

        /* If the stored line has no trailing newline (last line of file
         * without EOF newline), add one so lines don't run together. */
        if (!has_nl)
            putchar('\n');

        free(lines[i]);
    }
    free(lines);
    free(nums);
}

/* ------------------------------------------------------------------ */
/* count_nonempty_lines: forward pass over fd in fixed-size chunks,    */
/* counting lines that contain a non-whitespace character. Nothing is  */
/* stored, so memory use stays constant regardless of file size.       */
/* ------------------------------------------------------------------ */
static int count_nonempty_lines(int fd) {
    char    buf[CHUNK_SIZE];
    int     count       = 0;
    int     has_content = 0; /* current line has a non-space char */
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
    return count + has_content; /* last line may lack a trailing '\n' */
}

/* ------------------------------------------------------------------ */
/* emit_reverse_line: print one line (without '\n') during a reverse   */
/* read. Numbers count down, since lines arrive last-to-first.         */
/* ------------------------------------------------------------------ */
static void emit_reverse_line(const char *text, int flag_n, int *next_num) {
    if (flag_n && !is_empty_line(text))
        printf("%d %s\n", (*next_num)--, text);
    else
        printf("%s\n", text);
}

/* ------------------------------------------------------------------ */
/* peek_reverse_seekable: reads regular file backwards in chunks.      */
/*                                                                      */
/* With -n, a forward counting pass finds how many numbered lines the  */
/* file has, so the last line can be given its ORIGINAL number and the */
/* numbers counted down from there — no need to buffer the file.       */
/* ------------------------------------------------------------------ */
static void peek_reverse_seekable(int fd, int flag_n, int *line_num) {
    struct stat st;
    if (fstat(fd, &st) != 0) return;

    off_t filesize = st.st_size;
    if (filesize == 0) return;

    /* Number that the last non-empty line of this file will get */
    int total    = flag_n ? count_nonempty_lines(fd) : 0;
    int next_num = *line_num + total - 1;
    *line_num   += total; /* the next file continues after this one */

    char   buf[CHUNK_SIZE];
    off_t  pos      = filesize;
    char  *leftover = NULL; /* partial line carried over from previous chunk */

    while (pos > 0) {
        off_t   to_read   = (pos > CHUNK_SIZE) ? CHUNK_SIZE : pos;
        pos              -= to_read;

        lseek(fd, pos, SEEK_SET);
        ssize_t bytes_read = read(fd, buf, to_read);
        if (bytes_read <= 0) break;

        int i = (int)bytes_read - 1;

        /* Skip the trailing newline of the file on the very first chunk */
        if (pos + to_read == filesize && i >= 0 && buf[i] == '\n')
            i--;

        /* Walk the chunk backwards, extracting lines */
        while (i >= 0) {
            int end_idx = i;

            /* Find the newline that terminates this line */
            while (i >= 0 && buf[i] != '\n')
                i--;

            /* Piece of text: buf[i+1 .. end_idx] */
            int   piece_len = end_idx - i;  /* may be 0 for blank line */
            char *piece     = malloc(piece_len + 1);
            memcpy(piece, buf + i + 1, piece_len);
            piece[piece_len] = '\0';

            /* Append any leftover from a previous chunk to the right */
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
                /* We hit a '\n' — full_line is a complete line */
                emit_reverse_line(full_line, flag_n, &next_num);
                free(full_line);
                i--; /* move past the '\n' for next iteration */
            } else {
                /* Hit the start of the chunk — save as leftover */
                leftover = full_line;
            }
        }
    }

    /* Flush any text from the very beginning of the file. If nothing is
     * left over, the file began with '\n', so its first line is empty. */
    if (leftover != NULL) {
        emit_reverse_line(leftover, flag_n, &next_num);
        free(leftover);
    } else if (pos == 0) {
        emit_reverse_line("", flag_n, &next_num);
    }
}

/* ------------------------------------------------------------------ */
/* process_file: dispatcher                                            */
/* ------------------------------------------------------------------ */
static void process_file(const char *filename, int flag_n, int flag_r,
                         int *line_num) {
    /* ── stdin / "-" ────────────────────────────────────────────────── */
    if (filename == NULL || strcmp(filename, "-") == 0) {
        if (flag_r)
            peek_reverse_buffered(stdin, flag_n, line_num);
        else
            peek_forward(stdin, flag_n, line_num);
        clearerr(stdin); /* clear EOF so the shell loop can keep reading */
        return;
    }

    /* ── File existence / type checks ───────────────────────────────── */
    struct stat st;
    if (stat(filename, &st) != 0) {
        fprintf(stderr, "peek: no such file or directory\n");
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        fprintf(stderr, "peek: is a directory\n");
        return;
    }

    /* ── Dispatch ────────────────────────────────────────────────────── */
    if (flag_r) {
        if (S_ISREG(st.st_mode)) {
            /* Seekable regular file: read backwards with lseek chunks */
            int fd = open(filename, O_RDONLY);
            if (fd >= 0) {
                peek_reverse_seekable(fd, flag_n, line_num);
                close(fd);
            }
        } else {
            /* Non-seekable source (e.g. a FIFO): buffering is allowed */
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

/* ------------------------------------------------------------------ */
/* peek — main entry point                                             */
/* ------------------------------------------------------------------ */
void peek(int argc, char **argv) {
    int flag_n = 0;
    int flag_r = 0;
    int i      = 1;

    /* ── Parse flags: consume every arg that starts with '-' ────────── */
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

    /*
     * line_num is a single counter shared across all files so that
     * numbering continues sequentially (e.g., README has lines 1-2,
     * then Makefile continues from 3).
     */
    int line_num = 1;

    if (i == argc) {
        /* No filenames — stdin is always valid, read directly */
        process_file(NULL, flag_n, flag_r, &line_num);
        return;
    }

    /* ── Pass 1: validate every filename before printing anything ─── */
    int all_valid = 1;
    for (int j = i; j < argc; j++) {
        /* "-" is the stdin sentinel — always valid */
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

    /* ── Pass 2: only print content if every file was valid ─────── */
    if (all_valid) {
        for (; i < argc; i++)
            process_file(argv[i], flag_n, flag_r, &line_num);
    }
}
