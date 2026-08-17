#include "prompt.h"
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char input[1024];
    TokenList tokens;

    init_prompt();

    while (1) {
        display_prompt();

        // STEP 1: Read user input from stdin
        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("\n");
            break;
        }

        // STEP 2: Remove trailing newline
        input[strcspn(input, "\n")] = '\0';

        // STEP 3: Skip empty input
        if (strlen(input) == 0) {
            continue;
        }

        // STEP 4: TOKENIZE - Parse input into linked list of tokens
        if (tokenize(input, &tokens) != 0) {
            // Syntax error detected (error message printed inside tokenize)
            continue;
        }

        if (tokens.count == 0) {
            continue;
        }

        // STEP 5: Print parsed tokens (Linked list output)
        print_tokens(&tokens);

        // STEP 6: Clean up allocated linked list
        free_tokens(&tokens);
    }

    return 0;
}