#include "lexer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int tests_run = 0;
static int tests_passed = 0;

/* Check that tokenize() succeeds, token count matches, and each
   token's type-name and value match the expected arrays.            */
static void expect_tokens(const char *input, int expected_count,
                          const char *expected_types[],
                          const char *expected_values[]) {
  tests_run++;
  printf("  LEX  %-50s ", input);

  TokenList list;
  int ret = tokenize(input, &list);
  if (ret != 0) {
    printf("[FAIL] tokenize returned %d\n", ret);
    return;
  }
  if (list.count != expected_count) {
    printf("[FAIL] expected %d tokens, got %d\n", expected_count, list.count);
    free_tokens(&list);
    return;
  }
  Token *curr = list.head;
  for (int i = 0; i < expected_count; i++) {
    if (curr == NULL) {
      printf("[FAIL] list ended early at token %d\n", i);
      free_tokens(&list);
      return;
    }
    if (strcmp(token_type_name(curr->type), expected_types[i]) != 0) {
      printf("[FAIL] token %d: type '%s' != expected '%s'\n", i,
             token_type_name(curr->type), expected_types[i]);
      free_tokens(&list);
      return;
    }
    if (strcmp(curr->value, expected_values[i]) != 0) {
      printf("[FAIL] token %d: value '%s' != expected '%s'\n", i, curr->value,
             expected_values[i]);
      free_tokens(&list);
      return;
    }
    curr = curr->next;
  }
  free_tokens(&list);
  printf("[PASS]\n");
  tests_passed++;
}

/* Check that tokenize() fails (returns -1). */
static void expect_lex_error(const char *input) {
  tests_run++;
  printf("  LERR %-50s ", input);
  TokenList list;
  int ret = tokenize(input, &list);
  if (ret == 0) {
    printf("[FAIL] expected lex error but got %d tokens\n", list.count);
    free_tokens(&list);
    return;
  }
  printf("[PASS]\n");
  tests_passed++;
}

/* Tokenize then validate grammar; expect both to succeed. */
static void expect_grammar_ok(const char *input) {
  tests_run++;
  printf("  GOK  %-50s ", input);
  TokenList list;
  int ret = tokenize(input, &list);
  if (ret != 0) {
    printf("[FAIL] tokenize failed\n");
    return;
  }
  ret = validate_grammar(&list);
  if (ret != 0) {
    printf("[FAIL] grammar rejected\n");
    free_tokens(&list);
    return;
  }
  free_tokens(&list);
  printf("[PASS]\n");
  tests_passed++;
}

/* Tokenize then validate grammar; expect grammar to reject. */
static void expect_grammar_err(const char *input) {
  tests_run++;
  printf("  GERR %-50s ", input);
  TokenList list;
  int ret = tokenize(input, &list);
  if (ret != 0) {
    /* Lex error counts as rejected — still valid for "input is invalid" */
    printf("[PASS] (lex error)\n");
    tests_passed++;
    return;
  }
  ret = validate_grammar(&list);
  if (ret == 0) {
    printf("[FAIL] grammar accepted but should have rejected\n");
    free_tokens(&list);
    return;
  }
  free_tokens(&list);
  printf("[PASS]\n");
  tests_passed++;
}

/* ------------------------------------------------------------------ */
/* Test Suite                                                         */
/* ------------------------------------------------------------------ */

int main(void) {
  printf("=== COMPREHENSIVE SPEC COMPLIANCE TEST SUITE ===\n\n");

  /* ---- SECTION 1: WHITESPACE HANDLING ---- */
  printf("--- Whitespace ---\n");
  {
    /* Empty / whitespace-only → 0 tokens */
    const char *t0[] = {};
    const char *v0[] = {};
    expect_tokens("", 0, t0, v0);
    expect_tokens("   ", 0, t0, v0);
    expect_tokens("\t\t", 0, t0, v0);
    expect_tokens("   \t   ", 0, t0, v0);
  }
  {
    /* Whitespace between tokens is discarded */
    const char *t[] = {"TOKEN_WORD", "TOKEN_WORD", "TOKEN_WORD"};
    const char *v[] = {"ls", "-la", "/home"};
    expect_tokens("  ls   -la   /home  ", 3, t, v);
  }
  {
    /* Whitespace inside quotes is preserved */
    const char *t[] = {"TOKEN_WORD", "TOKEN_WORD"};
    const char *v[] = {"echo", "hello   world"};
    expect_tokens("echo 'hello   world'", 2, t, v);
  }

  /* ---- SECTION 2: OPERATOR TOKENS & MAXIMAL MUNCH ---- */
  printf("\n--- Operators & Maximal Munch ---\n");
  {
    /* All single-char operators */
    const char *t[] = {"OP_PIPE", "OP_AMP", "OP_GT", "OP_LT", "OP_SEMI"};
    const char *v[] = {"|", "&", ">", "<", ";"};
    expect_tokens("| & > < ;", 5, t, v);
  }
  {
    /* >> is one OP_GTGT, not two OP_GT */
    const char *t[] = {"OP_GTGT"};
    const char *v[] = {">>"};
    expect_tokens(">>", 1, t, v);
  }
  {
    /* << is NOT a multi-char operator per spec → two OP_LT */
    const char *t[] = {"OP_LT", "OP_LT"};
    const char *v[] = {"<", "<"};
    expect_tokens("<<", 2, t, v);
  }
  {
    /* && is NOT a multi-char operator per spec → two OP_AMP */
    const char *t[] = {"OP_AMP", "OP_AMP"};
    const char *v[] = {"&", "&"};
    expect_tokens("&&", 2, t, v);
  }
  {
    /* || is NOT a multi-char operator per spec → two OP_PIPE */
    const char *t[] = {"OP_PIPE", "OP_PIPE"};
    const char *v[] = {"|", "|"};
    expect_tokens("||", 2, t, v);
  }
  {
    /* >>> should be OP_GTGT + OP_GT (maximal munch takes >> first) */
    const char *t[] = {"OP_GTGT", "OP_GT"};
    const char *v[] = {">>", ">"};
    expect_tokens(">>>", 2, t, v);
  }
  {
    /* Operators adjacent to words */
    const char *t[] = {"TOKEN_WORD", "OP_PIPE", "TOKEN_WORD", "OP_GTGT",
                        "TOKEN_WORD"};
    const char *v[] = {"cat", "|", "grep", ">>", "out"};
    expect_tokens("cat|grep>>out", 5, t, v);
  }

  /* ---- SECTION 3: WORD FRAGMENTS ---- */
  printf("\n--- Word Fragments ---\n");
  {
    /* Ordinary characters */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"hello-world_123"};
    expect_tokens("hello-world_123", 1, t, v);
  }
  {
    /* Unquoted escape: \c → c literally, backslash removed */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"|"}; /* \| → | as literal WORD, not operator */
    expect_tokens("\\|", 1, t, v);
  }
  {
    /* Unquoted escape joins into a single word */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"hello world"};
    expect_tokens("hello\\ world", 1, t, v);
  }
  {
    /* Escaped special characters lose their operator meaning */
    const char *t[] = {"TOKEN_WORD", "TOKEN_WORD", "TOKEN_WORD"};
    const char *v[] = {">", "<", ";"};
    expect_tokens("\\> \\< \\;", 3, t, v);
  }

  /* ---- SECTION 4: SINGLE QUOTES ---- */
  printf("\n--- Single Quotes ---\n");
  {
    /* Basic single quotes: literal content */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"hello world"};
    expect_tokens("'hello world'", 1, t, v);
  }
  {
    /* Single quotes: backslash is literal, NOT an escape */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"hello\\nworld"};
    expect_tokens("'hello\\nworld'", 1, t, v);
  }
  {
    /* Single quotes: special chars are literal */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"| > < & ; >>"};
    expect_tokens("'| > < & ; >>'", 1, t, v);
  }
  {
    /* Single quotes: double quote inside is literal */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"he said \"hi\""};
    expect_tokens("'he said \"hi\"'", 1, t, v);
  }
  {
    /* Empty single quotes produce an empty WORD */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {""};
    expect_tokens("''", 1, t, v);
  }

  /* ---- SECTION 5: DOUBLE QUOTES ---- */
  printf("\n--- Double Quotes ---\n");
  {
    /* Basic double quotes */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"hello world"};
    expect_tokens("\"hello world\"", 1, t, v);
  }
  {
    /* \" inside double quotes → literal " */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"say \"hi\""};
    expect_tokens("\"say \\\"hi\\\"\"", 1, t, v);
  }
  {
    /* \\ inside double quotes → literal \ */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"path\\dir"};
    expect_tokens("\"path\\\\dir\"", 1, t, v);
  }
  {
    /* Other \c inside double quotes → both chars preserved */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"hello\\nworld"};
    expect_tokens("\"hello\\nworld\"", 1, t, v);
  }
  {
    /* Special chars inside double quotes lose operator meaning */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"a | b > c"};
    expect_tokens("\"a | b > c\"", 1, t, v);
  }
  {
    /* Empty double quotes produce an empty WORD */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {""};
    expect_tokens("\"\"", 1, t, v);
  }

  /* ---- SECTION 6: FRAGMENT CONCATENATION ---- */
  printf("\n--- Fragment Concatenation ---\n");
  {
    /* Adjacent fragments form one WORD: 'hel'"lo" → hello */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"hello"};
    expect_tokens("'hel'\"lo\"", 1, t, v);
  }
  {
    /* Mix ordinary + single-quote + double-quote */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"abcdef"};
    expect_tokens("ab'cd'\"ef\"", 1, t, v);
  }
  {
    /* Ordinary + escape + quoted */
    const char *t[] = {"TOKEN_WORD"};
    const char *v[] = {"a>b"};
    expect_tokens("a\\>'b'", 1, t, v);
  }

  /* ---- SECTION 7: LEXICAL ERRORS ---- */
  printf("\n--- Lexical Errors ---\n");
  expect_lex_error("echo 'unclosed");
  expect_lex_error("echo \"unclosed");
  expect_lex_error("echo \\");
  expect_lex_error("echo \"hello \\");
  expect_lex_error("'");
  expect_lex_error("\"");

  /* ---- SECTION 8: GRAMMAR — VALID INPUTS ---- */
  printf("\n--- Grammar: Valid ---\n");

  /* LINE → ε */
  expect_grammar_ok("");
  expect_grammar_ok("   ");

  /* LINE → WORD ARG, ARG → ε */
  expect_grammar_ok("ls");
  expect_grammar_ok("pwd");

  /* ARG → WORD ARG */
  expect_grammar_ok("ls -la /tmp");
  expect_grammar_ok("echo hello world");

  /* ARG → OP_LT TGT  (TGT → WORD ARG) */
  expect_grammar_ok("cat < file.txt");
  expect_grammar_ok("sort < data.txt -n");

  /* ARG → OP_GT TGT */
  expect_grammar_ok("echo hello > out.txt");
  expect_grammar_ok("ls > /tmp/list.txt");

  /* ARG → OP_GTGT TGT */
  expect_grammar_ok("echo log >> logfile.txt");
  expect_grammar_ok("date >> timestamps.log");

  /* ARG → OP_PIPE CMD  (CMD → WORD ARG) */
  expect_grammar_ok("ls | grep foo");
  expect_grammar_ok("cat f.txt | sort | uniq");

  /* ARG → OP_SEMI CMD */
  expect_grammar_ok("cd /tmp ; ls");
  expect_grammar_ok("echo a ; echo b ; echo c");

  /* ARG → OP_AMP BG  (BG → ε) */
  expect_grammar_ok("sleep 10 &");

  /* ARG → OP_AMP BG  (BG → WORD ARG) */
  expect_grammar_ok("sleep 10 & echo done");

  /* Chained redirections */
  expect_grammar_ok("cat < in.txt | grep foo > out.txt");
  expect_grammar_ok("sort < data.txt > sorted.txt");

  /* Quoted words in valid positions */
  expect_grammar_ok("echo 'hello world' | grep hello");
  expect_grammar_ok("echo \"hello\" > out.txt");

  /* ---- SECTION 9: GRAMMAR — INVALID INPUTS ---- */
  printf("\n--- Grammar: Invalid ---\n");

  /* LINE starts with operator (not WORD) */
  expect_grammar_err("| grep foo");
  expect_grammar_err("> out.txt");
  expect_grammar_err("; ls");
  expect_grammar_err("& sleep 10");
  expect_grammar_err("< file.txt");
  expect_grammar_err(">> out.txt");

  /* CMD must start with WORD (missing command after | or ;) */
  expect_grammar_err("ls |");
  expect_grammar_err("ls ;");
  expect_grammar_err("ls | | grep");
  expect_grammar_err("ls ; ;");

  /* TGT must start with WORD (missing filename after redirect) */
  expect_grammar_err("echo >");
  expect_grammar_err("echo >>");
  expect_grammar_err("echo <");
  expect_grammar_err("cat > | grep");  /* | is not a WORD */
  expect_grammar_err("cat >> ;");      /* ; is not a WORD */
  expect_grammar_err("cat < >");       /* > is not a WORD */

  /* && is two OP_AMP tokens → first & triggers BG, second & is
     not WORD so BG rejects it */
  expect_grammar_err("ls && echo hi");

  /* || is two OP_PIPE tokens → first | triggers CMD, second | is
     not WORD so CMD rejects it */
  expect_grammar_err("ls || echo hi");

  /* << is two OP_LT → first < triggers TGT, second < is not WORD */
  expect_grammar_err("cat << EOF");

  /* ---- SUMMARY ---- */
  printf("\n=== RESULTS: %d/%d PASSED ===\n", tests_passed, tests_run);
  return tests_passed == tests_run ? 0 : 1;
}
