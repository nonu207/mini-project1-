#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

/**
 * Token types recognized by the lexer
 */
typedef enum {
    TOKEN_WORD,        // Words, arguments, literal text
    TOKEN_OP_PIPE,     // |
    TOKEN_OP_AMP,      // &
    TOKEN_OP_GT,       // >
    TOKEN_OP_GTGT,     // >>
    TOKEN_OP_LT,       // <
    TOKEN_OP_LTLT,     // <<
    TOKEN_OP_AMPAMP,   // &&
    TOKEN_OP_PIPEPIPE, // ||
    TOKEN_OP_SEMI      // ;
} TokenType;

/**
 * Linked list node representing a single token
 */
typedef struct TokenNode {
    TokenType type;          // Enum type of token
    char *value;             // Value of token (dynamically allocated string)
    struct TokenNode *next;  // Pointer to the next token node
} Token;

/**
 * Linked list container for parsed tokens
 */
typedef struct {
    Token *head;  // Pointer to first node in linked list
    Token *tail;  // Pointer to last node in linked list
    int count;    // Total number of tokens in list
} TokenList;

/**
 * Returns string representation of token type (e.g., "OP_PIPE", "OP_GTGT", "TOKEN_WORD")
 */
const char *token_type_name(TokenType type);

/**
 * Tokenize input string into a linked list of tokens.
 *
 * Iterates through input character by character.
 * Discards whitespace unless inside quotes.
 * Uses maximal munch for multi-character operators (e.g. >> -> OP_GTGT).
 * Handles single quotes (literal), double quotes (\", \\), and unquoted escapes (\c).
 * Rejects unclosed quotes or trailing backslashes with: "cshell: invalid syntax".
 *
 * @param input: Raw command line input string
 * @param token_list: Pointer to TokenList structure to store head/tail pointers
 * @return: 0 on success, -1 on lexical syntax error
 */
int tokenize(const char *input, TokenList *token_list);

/**
 * Free all allocated nodes and values in the token linked list.
 *
 * @param token_list: Pointer to TokenList structure to clean up
 */
void free_tokens(TokenList *token_list);

/**
 * Print all tokens in the linked list (useful for debugging and verification).
 *
 * @param token_list: Pointer to TokenList structure to print
 */
void print_tokens(const TokenList *token_list);

#endif // LEXER_H
