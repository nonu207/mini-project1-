#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_case(const char *input, int expected_ret, int expected_count, const char *expected_types[], const char *expected_values[]) {
    printf("Testing input: \"%s\"\n", input);
    TokenList list;
    int ret = tokenize(input, &list);
    assert(ret == expected_ret);

    if (ret == 0) {
        assert(list.count == expected_count);
        Token *curr = list.head;
        for (int i = 0; i < expected_count; i++) {
            assert(curr != NULL);
            assert(strcmp(token_type_name(curr->type), expected_types[i]) == 0);
            assert(strcmp(curr->value, expected_values[i]) == 0);
            curr = curr->next;
        }
        assert(curr == NULL); // End of list
    }
    free_tokens(&list);
    printf("  [PASS]\n");
}

int main(void) {
    printf("=== RUNNING LEXER TEST SUITE ===\n\n");

    // Test 1: Basic words and whitespace
    {
        const char *t[] = {"TOKEN_WORD", "TOKEN_WORD", "TOKEN_WORD"};
        const char *v[] = {"ls", "-la", "/home"};
        test_case("  ls   -la   /home  ", 0, 3, t, v);
    }

    // Test 2: Special Characters and Maximal Munch
    {
        const char *t[] = {"TOKEN_WORD", "OP_PIPE", "TOKEN_WORD", "OP_GTGT", "TOKEN_WORD", "OP_AMP", "OP_SEMI"};
        const char *v[] = {"cat", "|", "grep", ">>", "out.txt", "&", ";"};
        test_case("cat | grep >> out.txt & ;", 0, 7, t, v);
    }

    // Test 3: Maximal munch check (>> vs >)
    {
        const char *t[] = {"OP_GTGT", "OP_GT", "OP_LTLT", "OP_LT", "OP_AMPAMP", "OP_PIPEPIPE"};
        const char *v[] = {">>", ">", "<<", "<", "&&", "||"};
        test_case(">> > << < && ||", 0, 6, t, v);
    }

    // Test 4: Single Quotes (literal contents, backslashes inside stay as backslashes)
    {
        const char *t[] = {"TOKEN_WORD", "TOKEN_WORD"};
        const char *v[] = {"echo", "hello | >> world \\n "};
        test_case("echo 'hello | >> world \\n '", 0, 2, t, v);
    }

    // Test 5: Double Quotes (\", \\ escape handling)
    {
        const char *t[] = {"TOKEN_WORD", "TOKEN_WORD"};
        const char *v[] = {"echo", "hello \"world\" \\ test"};
        test_case("echo \"hello \\\"world\\\" \\\\ test\"", 0, 2, t, v);
    }

    // Test 6: Unquoted Escapes (\c)
    {
        const char *t[] = {"TOKEN_WORD", "TOKEN_WORD", "TOKEN_WORD"};
        const char *v[] = {"echo", "hello world", ">"};
        test_case("echo hello\\ world \\>", 0, 3, t, v);
    }

    // Test 7: Lexical Errors - Unclosed single quote
    {
        printf("Expecting error output below:\n");
        test_case("echo 'unclosed single quote", -1, 0, NULL, NULL);
    }

    // Test 8: Lexical Errors - Unclosed double quote
    {
        printf("Expecting error output below:\n");
        test_case("echo \"unclosed double quote", -1, 0, NULL, NULL);
    }

    // Test 9: Lexical Errors - Trailing backslash
    {
        printf("Expecting error output below:\n");
        test_case("echo trailing backslash\\", -1, 0, NULL, NULL);
    }

    printf("\n=== ALL LEXER TESTS PASSED SUCCESSFULLY! ===\n");
    return 0;
}
