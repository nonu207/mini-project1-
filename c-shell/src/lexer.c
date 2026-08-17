#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/**
 * Helper to convert TokenType enum to string representation
 */
const char *token_type_name(TokenType type) {
    switch (type) {
        case TOKEN_WORD:        return "TOKEN_WORD";
        case TOKEN_OP_PIPE:     return "OP_PIPE";
        case TOKEN_OP_AMP:      return "OP_AMP";
        case TOKEN_OP_GT:       return "OP_GT";
        case TOKEN_OP_GTGT:     return "OP_GTGT";
        case TOKEN_OP_LT:       return "OP_LT";
        case TOKEN_OP_LTLT:     return "OP_LTLT";
        case TOKEN_OP_AMPAMP:   return "OP_AMPAMP";
        case TOKEN_OP_PIPEPIPE: return "OP_PIPEPIPE";
        case TOKEN_OP_SEMI:     return "OP_SEMI";
        default:                return "UNKNOWN";
    }
}

/**
 * Allocate and initialize a new Token linked list node
 */
static Token *create_token_node(TokenType type, const char *value) {
    Token *node = (Token *)malloc(sizeof(Token));
    if (node == NULL) {
        perror("malloc");
        return NULL;
    }
    node->type = type;
    node->value = strdup(value ? value : "");
    if (node->value == NULL) {
        perror("strdup");
        free(node);
        return NULL;
    }
    node->next = NULL;
    return node;
}

/**
 * Append a node to the end of the TokenList linked list
 */
static void append_token(TokenList *token_list, Token *node) {
    if (token_list == NULL || node == NULL) {
        return;
    }
    if (token_list->tail == NULL) {
        token_list->head = node;
        token_list->tail = node;
    } else {
        token_list->tail->next = node;
        token_list->tail = node;
    }
    token_list->count++;
}

/**
 * Tokenize input string into a linked list of tokens
 */
int tokenize(const char *input, TokenList *token_list) {
    if (input == NULL || token_list == NULL) {
        return -1;
    }

    // Initialize token list
    token_list->head = NULL;
    token_list->tail = NULL;
    token_list->count = 0;

    const char *p = input;

    while (*p != '\0') {
        // 1. Handle Whitespace: Spaces, tabs, newlines outside quotes are ignored
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        // 2. Identify Special Characters / Operators with "Maximal Munch"
        if (*p == '|' || *p == '&' || *p == '>' || *p == '<' || *p == ';') {
            TokenType op_type;
            char op_str[3] = {0};

            if (*p == '>' && *(p + 1) == '>') {
                op_type = TOKEN_OP_GTGT;
                op_str[0] = '>';
                op_str[1] = '>';
                p += 2;
            } else if (*p == '>') {
                op_type = TOKEN_OP_GT;
                op_str[0] = '>';
                p += 1;
            } else if (*p == '<' && *(p + 1) == '<') {
                op_type = TOKEN_OP_LTLT;
                op_str[0] = '<';
                op_str[1] = '<';
                p += 2;
            } else if (*p == '<') {
                op_type = TOKEN_OP_LT;
                op_str[0] = '<';
                p += 1;
            } else if (*p == '|' && *(p + 1) == '|') {
                op_type = TOKEN_OP_PIPEPIPE;
                op_str[0] = '|';
                op_str[1] = '|';
                p += 2;
            } else if (*p == '|') {
                op_type = TOKEN_OP_PIPE;
                op_str[0] = '|';
                p += 1;
            } else if (*p == '&' && *(p + 1) == '&') {
                op_type = TOKEN_OP_AMPAMP;
                op_str[0] = '&';
                op_str[1] = '&';
                p += 2;
            } else if (*p == '&') {
                op_type = TOKEN_OP_AMP;
                op_str[0] = '&';
                p += 1;
            } else if (*p == ';') {
                op_type = TOKEN_OP_SEMI;
                op_str[0] = ';';
                p += 1;
            } else {
                p++;
                continue;
            }

            Token *node = create_token_node(op_type, op_str);
            if (node == NULL) {
                free_tokens(token_list);
                return -1;
            }
            append_token(token_list, node);
            continue;
        }

        // 3. Parse WORD token (Handles Quotes & Escapes character by character)
        size_t buf_capacity = 64;
        size_t buf_len = 0;
        char *buf = (char *)malloc(buf_capacity);
        if (buf == NULL) {
            perror("malloc");
            free_tokens(token_list);
            return -1;
        }

        int syntax_error = 0;

        while (*p != '\0') {
            // Unquoted whitespace or operator marks the end of the WORD
            if (isspace((unsigned char)*p) || *p == '|' || *p == '&' || *p == '>' || *p == '<' || *p == ';') {
                break;
            }

            // Case A: Unquoted Escapes (\c)
            if (*p == '\\') {
                p++; // Skip backslash
                if (*p == '\0') {
                    // Trailing backslash at line end
                    syntax_error = 1;
                    break;
                }
                // Append next character literally
                if (buf_len + 1 >= buf_capacity) {
                    buf_capacity *= 2;
                    char *new_buf = (char *)realloc(buf, buf_capacity);
                    if (new_buf == NULL) {
                        perror("realloc");
                        free(buf);
                        free_tokens(token_list);
                        return -1;
                    }
                    buf = new_buf;
                }
                buf[buf_len++] = *p;
                p++;
                continue;
            }

            // Case B: Single Quotes ('...') - Everything inside is literal text
            if (*p == '\'') {
                p++; // Skip opening single quote
                while (*p != '\0' && *p != '\'') {
                    if (buf_len + 1 >= buf_capacity) {
                        buf_capacity *= 2;
                        char *new_buf = (char *)realloc(buf, buf_capacity);
                        if (new_buf == NULL) {
                            perror("realloc");
                            free(buf);
                            free_tokens(token_list);
                            return -1;
                        }
                        buf = new_buf;
                    }
                    buf[buf_len++] = *p;
                    p++;
                }
                if (*p == '\0') {
                    // Unclosed single quote before line ends
                    syntax_error = 1;
                    break;
                }
                p++; // Skip closing single quote
                continue;
            }

            // Case C: Double Quotes ("...") - Process \" and \\ escapes
            if (*p == '"') {
                p++; // Skip opening double quote
                while (*p != '\0' && *p != '"') {
                    if (*p == '\\') {
                        if (*(p + 1) == '\0') {
                            syntax_error = 1;
                            break;
                        }
                        // If escape is \" or \\, remove backslash and treat next char literally
                        if (*(p + 1) == '"' || *(p + 1) == '\\') {
                            p++; // Skip backslash
                        }
                    }
                    if (buf_len + 1 >= buf_capacity) {
                        buf_capacity *= 2;
                        char *new_buf = (char *)realloc(buf, buf_capacity);
                        if (new_buf == NULL) {
                            perror("realloc");
                            free(buf);
                            free_tokens(token_list);
                            return -1;
                        }
                        buf = new_buf;
                    }
                    buf[buf_len++] = *p;
                    p++;
                }
                if (syntax_error || *p == '\0') {
                    syntax_error = 1;
                    break;
                }
                p++; // Skip closing double quote
                continue;
            }

            // Case D: Regular character inside word
            if (buf_len + 1 >= buf_capacity) {
                buf_capacity *= 2;
                char *new_buf = (char *)realloc(buf, buf_capacity);
                if (new_buf == NULL) {
                    perror("realloc");
                    free(buf);
                    free_tokens(token_list);
                    return -1;
                }
                buf = new_buf;
            }
            buf[buf_len++] = *p;
            p++;
        }

        if (syntax_error) {
            free(buf);
            free_tokens(token_list);
            fprintf(stderr, "cshell: invalid syntax\n");
            return -1;
        }

        buf[buf_len] = '\0';
        Token *node = create_token_node(TOKEN_WORD, buf);
        free(buf);

        if (node == NULL) {
            free_tokens(token_list);
            return -1;
        }
        append_token(token_list, node);
    }

    return 0;
}

/**
 * Free all allocated token memory in linked list
 */
void free_tokens(TokenList *token_list) {
    if (token_list == NULL) {
        return;
    }

    Token *current = token_list->head;
    while (current != NULL) {
        Token *next = current->next;
        if (current->value != NULL) {
            free(current->value);
            current->value = NULL;
        }
        free(current);
        current = next;
    }

    token_list->head = NULL;
    token_list->tail = NULL;
    token_list->count = 0;
}

/**
 * Print all tokens in the linked list
 */
void print_tokens(const TokenList *token_list) {
    if (token_list == NULL || token_list->head == NULL) {
        printf("No tokens\n");
        return;
    }

    printf("Parsed %d tokens:\n", token_list->count);
    int i = 0;
    for (const Token *curr = token_list->head; curr != NULL; curr = curr->next, i++) {
        printf("  [%d] %-12s: \"%s\"\n", i, token_type_name(curr->type), curr->value);
    }
}
