#include "shell.h"

/* Returns a human-readable name for a token type, used for debugging. */
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

/* Allocates a new token node with the given type and a copy of value. */
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

/* Appends a token node to the end of a token list. */
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

/* Splits an input line into a list of tokens: words and the operators
 * pipe, ampersand, greater-than, double greater-than, less-than and
 * semicolon. Whitespace outside quotes separates tokens and is
 * otherwise ignored. Operators are recognized with maximal munch, so
 * two consecutive greater-than characters are read as one append-
 * redirection token rather than two separate ones.
 *
 * Inside a word, a backslash escapes the character that follows it
 * literally, single quotes take everything inside them as literal
 * text with no escapes, and double quotes take everything inside them
 * literally except for an escaped quote or an escaped backslash. A
 * trailing backslash at the end of the line, or an unclosed single or
 * double quote, is a syntax error.
 *
 * Returns 0 on success, or -1 on a syntax error or allocation failure,
 * after printing an error message and freeing any tokens already
 * collected. */
int tokenize(const char *input, TokenList *token_list) {
  if (input == NULL || token_list == NULL) {
    return -1;
  }

  token_list->head = NULL;
  token_list->tail = NULL;
  token_list->count = 0;

  const char *p = input;

  while (*p != '\0') {
    while (*p != '\0' && isspace((unsigned char)*p)) {
      p++;
    }

    if (*p == '\0') {
      break;
    }

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

    /* A word is built up character by character into a growable
     * buffer, since its length is not known in advance. */
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
      /* Unquoted whitespace or an operator character ends the word. */
      if (isspace((unsigned char)*p) || *p == '|' || *p == '&' || *p == '>' ||
          *p == '<' || *p == ';') {
        break;
      }

      /* An unquoted backslash escapes the next character literally. */
      if (*p == '\\') {
        p++;
        if (*p == '\0') {
          syntax_error = 1;
          break;
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
        buf[buf_len] = *p;
        buf_len += 1;
        p++;
        continue;
      }

      /* Inside single quotes, everything up to the closing quote is
       * taken literally, with no escapes recognized. */
      if (*p == '\'') {
        p++;
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
          syntax_error = 1;
          break;
        }
        p++;
        continue;
      }

      /* Inside double quotes, an escaped quote or an escaped backslash
       * is unescaped; anything else is taken literally. */
      if (*p == '"') {
        p++;
        while (*p != '\0' && *p != '"') {
          if (*p == '\\') {
            if (*(p + 1) == '\0') {
              syntax_error = 1;
              break;
            }

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
        p++;
        continue;
      }

      /* An ordinary character inside a word. */
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

/* Frees every token node in the list and resets it to empty. This must
 * be called after each command line is processed, or the memory for
 * its tokens and their string values would leak. */
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

/* Grammar validator.
 *
 * Implements a recursive descent check over the token list for this
 * grammar:
 *
 *   LINE  -> epsilon | WORD ARG
 *   ARG   -> epsilon | WORD ARG | OP_LT TGT | OP_GT TGT | OP_GTGT TGT
 *                     | OP_PIPE CMD | OP_SEMI CMD | OP_AMP BG
 *   CMD   -> WORD ARG
 *   TGT   -> WORD ARG
 *   BG    -> epsilon | WORD ARG
 *
 * Each parse function below takes a cursor into the token list and
 * advances it as tokens are consumed. Every function returns 0 on
 * success or -1 on a grammar error. */

static int parse_arg(Token **curr);
static int parse_cmd(Token **curr);
static int parse_tgt(Token **curr);
static int parse_bg(Token **curr);

/* Prints the fixed grammar error message and returns -1. The token
 * that triggered the error is accepted as an argument for future
 * debugging, but the spec requires the same fixed message regardless
 * of where the error occurred. */
static int grammar_error(const char *near) {
  (void)near;
  fprintf(stderr, "cshell: invalid syntax\n");
  return -1;
}

/* Parses the ARG rule: after the first word, the rest of the line may
 * be empty, another word, a redirection followed by its target, or a
 * pipe, semicolon or ampersand followed by the next command. Any other
 * operator appearing here has nothing valid to follow it and is a
 * grammar error. */
static int parse_arg(Token **curr) {
  if (*curr == NULL) {
    return 0;
  }

  switch ((*curr)->type) {

  case TOKEN_WORD:
    *curr = (*curr)->next;
    return parse_arg(curr);

  case TOKEN_OP_LT:
  case TOKEN_OP_GT:
  case TOKEN_OP_GTGT:
    *curr = (*curr)->next;
    return parse_tgt(curr);

  case TOKEN_OP_PIPE:
  case TOKEN_OP_SEMI:
    *curr = (*curr)->next;
    return parse_cmd(curr);

  case TOKEN_OP_AMP:
    *curr = (*curr)->next;
    return parse_bg(curr);

  default:
    return grammar_error((*curr)->value);
  }
}

/* Parses the CMD rule, used after a pipe or semicolon: the next token
 * must be a word naming a command, followed by its own arguments. */
static int parse_cmd(Token **curr) {
  if (*curr == NULL || (*curr)->type != TOKEN_WORD) {
    return grammar_error(*curr ? (*curr)->value : NULL);
  }
  *curr = (*curr)->next;
  return parse_arg(curr);
}

/* Parses the TGT rule, used after a redirection operator: the next
 * token must be a word naming the redirection target. */
static int parse_tgt(Token **curr) {
  if (*curr == NULL || (*curr)->type != TOKEN_WORD) {
    return grammar_error(*curr ? (*curr)->value : NULL);
  }
  *curr = (*curr)->next;
  return parse_arg(curr);
}

/* Parses the BG rule, used after an ampersand: the rest of the line is
 * optional, but if anything follows it must be a word starting a new
 * command. Any operator directly after an ampersand is a grammar
 * error. */
static int parse_bg(Token **curr) {
  if (*curr == NULL) {
    return 0;
  }
  if ((*curr)->type == TOKEN_WORD) {
    *curr = (*curr)->next;
    return parse_arg(curr);
  }
  return grammar_error((*curr)->value);
}

/* Public entry point for grammar validation. An empty line is valid;
 * otherwise the line must begin with a word. */
int validate_grammar(const TokenList *token_list) {
  if (token_list == NULL || token_list->head == NULL) {
    return 0;
  }

  Token *curr = token_list->head;

  if (curr->type != TOKEN_WORD) {
    return grammar_error(curr->value);
  }
  curr = curr->next;
  return parse_arg(&curr);
}

/* Returns the text form of a redirection or pipe operator, or NULL for
 * any other token type. */
static const char *op_text(TokenType type) {
  switch (type) {
  case TOKEN_OP_PIPE: return "|";
  case TOKEN_OP_GT:   return ">";
  case TOKEN_OP_GTGT: return ">>";
  case TOKEN_OP_LT:   return "<";
  default:            return NULL;
  }
}

/* Rebuilds one command group's text as the user originally typed it,
 * up to the next semicolon or ampersand, writing the result into out.
 * Used to display or record a job's command line, for example in the
 * activities and resume output. If the result would not fit in the
 * buffer, it is truncated rather than overflowing. */
void token_group_to_string(Token *start, char *out, size_t size) {
  if (out == NULL || size == 0)
    return;
  out[0] = '\0';

  size_t len = 0;
  for (Token *t = start; t != NULL; t = t->next) {
    if (t->type == TOKEN_OP_SEMI || t->type == TOKEN_OP_AMP)
      break;

    const char *piece = (t->type == TOKEN_WORD) ? t->value : op_text(t->type);
    if (piece == NULL)
      continue;

    size_t plen = strlen(piece);
    if (len + plen + (len ? 1 : 0) + 1 > size)
      break;
    if (len)
      out[len++] = ' ';
    memcpy(out + len, piece, plen);
    len += plen;
    out[len] = '\0';
  }
}
