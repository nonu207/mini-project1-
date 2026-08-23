#include "shell.h"

const char *token_type_name(TokenType type) {
  switch (type) {
  case TOKEN_WORD:
    return "TOKEN_WORD";
  case TOKEN_OP_PIPE:
    return "OP_PIPE";
  case TOKEN_OP_AMP:
    return "OP_AMP";
  case TOKEN_OP_GT:
    return "OP_GT";
  case TOKEN_OP_GTGT:
    return "OP_GTGT";
  case TOKEN_OP_LT:
    return "OP_LT";
  case TOKEN_OP_SEMI:
    return "OP_SEMI";
  default:
    return "UNKNOWN";
  }
}

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

int tokenize(const char *input, TokenList *token_list) {
  if (input == NULL || token_list == NULL) {
    return -1;
  }

  // Initialize token list
  token_list->head = NULL;
  token_list->tail = NULL;
  token_list->count = 0;

  // Pointer to current position in input string that gets moved forward as
  // tokens are processed used cuz look ahead is clean and convenient and for
  // operators we need to check if its >> or > or << or < etc. p will be updated
  // as we scan the input string and hence we dont need a separate index
  // variable
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
    // Per spec: special -> | & > < ;   Only >> is a multi-char operator.
    if (*p == '|' || *p == '&' || *p == '>' || *p == '<' || *p == ';') {
      TokenType op_type;
      char op_str[3] = {0};

      if (*p == '>' && *(p + 1) == '>') {
        // Maximal munch: >> is one OP_GTGT token, not two OP_GT
        op_type = TOKEN_OP_GTGT;
        op_str[0] = '>';
        op_str[1] = '>';
        p += 2;
      } else if (*p == '>') {
        op_type = TOKEN_OP_GT;
        op_str[0] = '>';
        p += 1;
      } else if (*p == '<') {
        op_type = TOKEN_OP_LT;
        op_str[0] = '<';
        p += 1;
      } else if (*p == '|') {
        op_type = TOKEN_OP_PIPE;
        op_str[0] = '|';
        p += 1;
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
    size_t buf_capacity = 64; // initially buffer size is 64 bytes
    size_t buf_len = 0;       // length of the buffer
    char *buf = (char *)malloc(buf_capacity);
    if (buf == NULL) {
      perror("malloc");
      free_tokens(token_list);
      return -1;
    }

    int syntax_error = 0;

    while (*p != '\0') {
      // Unquoted whitespace or operator marks the end of the WORD
      if (isspace((unsigned char)*p) || *p == '|' || *p == '&' || *p == '>' ||
          *p == '<' || *p == ';') {
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
        // dynamic buffer, ensures that program does not overflow memory while
        // building tokens of unknown length
        if (buf_len + 1 >= buf_capacity) {
          buf_capacity *=
              2; // achieves O(1) amortized time complexity, otherwise will have
                 // to call realloc again and again and program will be slow
          char *new_buf = (char *)realloc(buf, buf_capacity);
          if (new_buf == NULL) {
            perror("realloc");
            free(buf);
            free_tokens(token_list);
            return -1;
          }
          buf = new_buf;
        }
        buf[buf_len] = *p;
        buf_len += 1;
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
          buf[buf_len] = *p;
          buf_len += 1;
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

            // this part handles double backlash and also when /", in both we
            // skip the first backlash
            if (*(p + 1) == '"' || *(p + 1) == '\\') {
              p++;
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
// without this every other command which is entered will leave an orphaned
// memory on the heap, leading to memory leaks which can cause program to
// eventually crash basically it cleans up the heap by removing all the
// allocated memory
void free_tokens(TokenList *token_list) {
  if (token_list == NULL) {
    return;
  }

  Token *current = token_list->head;
  while (current != NULL) {
    Token *next = current->next;
    if (current->value != NULL) {
      free(current->value); // without this string memory for a token is lost
                            // forever in RAM
      current->value = NULL;
    }
    free(current);
    current = next;
  }

  token_list->head = NULL;
  token_list->tail = NULL;
  token_list->count = 0;
}

// void print_tokens(const TokenList *token_list) {
//   if (token_list == NULL || token_list->head == NULL) {
//     printf("No tokens\n");
//     return;
//   }
//   printf("Parsed %d tokens:\n", token_list->count);
//   int i = 0;
//   for (const Token *curr = token_list->head; curr != NULL;
//        curr = curr->next, i++) {
//     printf("  [%d] %-12s: \"%s\"\n", i, token_type_name(curr->type),
//            curr->value);
//   }
// }

/* -----------------------------------------------------------------------
 * Grammar Validator
 *
 * Implements a recursive descent validator over the token linked list for:
 *
 *   LINE  ->  epsilon | WORD ARG
 *   ARG   ->  epsilon | WORD ARG | OP_LT TGT | OP_GT TGT | OP_GTGT TGT
 *                     | OP_PIPE CMD | OP_SEMI CMD | OP_AMP BG
 *   CMD   ->  WORD ARG
 *   TGT   ->  WORD ARG
 *   BG    ->  epsilon | WORD ARG
 *
 * Each parse_*() function receives a Token** cursor and advances it as
 * tokens are consumed.  Returns 0 on success, -1 on grammar error.
 * ----------------------------------------------------------------------- */

/* Forward declarations for mutual recursion */
static int parse_arg(Token **curr); // argument following the first word
static int parse_cmd(Token **curr); // another command after | or ;
static int parse_tgt(Token **curr); // target of redirection
static int parse_bg(Token **curr);  // background command

/* Helper: emit a grammar error and return -1 */
static int grammar_error(const char *near) {
  (void)near; /* near is available for debugging but spec requires fixed msg */
  fprintf(stderr, "cshell: invalid syntax\n");
  return -1;
}

/*
 * ARG -> epsilon
 *      | WORD    ARG
 *      | OP_LT   TGT
 *      | OP_GT   TGT
 *      | OP_GTGT TGT
 *      | OP_PIPE CMD
 *      | OP_SEMI CMD
 *      | OP_AMP  BG
 */
static int parse_arg(Token **curr) {
  /* ARG -> epsilon : nothing more to consume, valid */
  if (*curr == NULL) {
    return 0;
  }

  switch ((*curr)->type) {

  /* ARG -> WORD ARG */
  case TOKEN_WORD:
    *curr = (*curr)->next; /* consume WORD */
    return parse_arg(curr);

  /* ARG -> OP_LT TGT  or  OP_GT TGT  or  OP_GTGT TGT */
  case TOKEN_OP_LT:
  case TOKEN_OP_GT:
  case TOKEN_OP_GTGT:
    *curr = (*curr)->next; /* consume the redirect operator */
    return parse_tgt(curr);

  /* ARG -> OP_PIPE CMD  or  OP_SEMI CMD */
  case TOKEN_OP_PIPE:
  case TOKEN_OP_SEMI:
    *curr = (*curr)->next; /* consume | or ; */
    return parse_cmd(curr);

  /* ARG -> OP_AMP BG */
  case TOKEN_OP_AMP:
    *curr = (*curr)->next; /* consume & */
    return parse_bg(curr);

  /* Anything else (OP_LTLT, OP_AMPAMP, OP_PIPEPIPE) cannot legally appear
   * in ARG position without a preceding WORD, so it is a grammar error. */
  default:
    return grammar_error((*curr)->value);
  }
}

/*
 * CMD -> WORD ARG
 * (used after OP_PIPE and OP_SEMI — must start with a WORD)
 */
static int parse_cmd(Token **curr) {
  if (*curr == NULL || (*curr)->type != TOKEN_WORD) {
    /* Nothing or a non-WORD token where a command name is required */
    return grammar_error(*curr ? (*curr)->value : NULL);
  }
  *curr = (*curr)->next; /* consume WORD */
  return parse_arg(curr);
}

/*
 * TGT -> WORD ARG
 * (used after OP_LT / OP_GT / OP_GTGT — must name a file/word)
 */
static int parse_tgt(Token **curr) {
  if (*curr == NULL || (*curr)->type != TOKEN_WORD) {
    return grammar_error(*curr ? (*curr)->value : NULL);
  }
  *curr = (*curr)->next; /* consume WORD */
  return parse_arg(curr);
}

/*
 * BG -> epsilon | WORD ARG
 * (used after OP_AMP — the rest of the line is optional)
 */
static int parse_bg(Token **curr) {
  if (*curr == NULL) {
    return 0; /* BG -> epsilon */
  }
  if ((*curr)->type == TOKEN_WORD) {
    *curr = (*curr)->next; /* consume WORD */
    return parse_arg(curr);
  }
  /* Any operator token directly after & is a grammar error */
  return grammar_error((*curr)->value);
}

/*
 * LINE -> epsilon | WORD ARG
 * Public entry point.
 */
int validate_grammar(const TokenList *token_list) {
  if (token_list == NULL || token_list->head == NULL) {
    return 0; /* LINE -> epsilon */
  }

  Token *curr = token_list->head;

  /* LINE must begin with a WORD */
  if (curr->type != TOKEN_WORD) {
    return grammar_error(curr->value);
  }
  curr = curr->next; /* consume leading WORD */
  return parse_arg(&curr);
}
