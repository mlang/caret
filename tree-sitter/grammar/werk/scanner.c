#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#define usize size_t;

///////////
// Types //
///////////

enum TokenType {
  TERMINATOR,

  TOKEN_TYPE_COUNT
};

/////////////
// Scanner //
/////////////

typedef struct Scanner {
  bool _;
} Scanner;

void *tree_sitter_werk_external_scanner_create() {
  Scanner *s = ts_malloc(sizeof(Scanner));

  return s;
}

void tree_sitter_werk_external_scanner_destroy(void *payload) { ts_free((Scanner *)(payload)); }

unsigned tree_sitter_werk_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *scanner = (Scanner *)payload;
  Scanner *buf = (Scanner *)buffer;

  memcpy(buf, scanner, sizeof(Scanner));
  return sizeof(Scanner);
}

void tree_sitter_werk_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *scanner = (Scanner *)payload;
  Scanner *buf = (Scanner *)buffer;

  memcpy(scanner, buf, length);
}

///////////
// Utils //
///////////

static int32_t cursor(TSLexer *lexer) { return lexer->lookahead; }
static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }
static void advance(TSLexer *lexer, Scanner *scanner) { lexer->advance(lexer, false); }
static void checkpoint(TSLexer *lexer, Scanner *scanner) { lexer->mark_end(lexer); }

static bool consume_until(TSLexer *lexer, Scanner *scanner, char c) {
  while (cursor(lexer) != c) {
    if (lexer->eof(lexer)) {
      return false;
    }
    advance(lexer, scanner);
  }
  return true;
}

static bool match_str_now(TSLexer *lexer, Scanner *scanner, char *str) {
  checkpoint(lexer, scanner);
  int32_t i = 0;

  for (;;) {
    if (lexer->eof(lexer)) return false;
    if (str[i] == '\0') return true;
    if (cursor(lexer) != str[i]) return false;

    i++;
    advance(lexer, scanner);
  }
}

static void print_init_log(TSLexer *lexer) {
  // lexer->log(lexer, "enter external scanner");
}

static void trim_leading_whitespace(TSLexer *lexer) {
  // lexer->log(lexer, "start at '%lc'", cursor(lexer));
  while (cursor(lexer) == ' ' || cursor(lexer) == '\t' || cursor(lexer) == '\r') {
    skip(lexer);
  }
  // lexer->log(lexer, "start at '%lc' after skipping whitespace", cursor(lexer));
}

bool tree_sitter_werk_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  Scanner *scanner = (Scanner *)payload;
  print_init_log(lexer);

  bool start_at_col_0 = lexer->get_column(lexer) == 0;

  trim_leading_whitespace(lexer);

  if (valid_symbols[TERMINATOR]) {
    if (cursor((lexer)) == '\n') {
      lexer->result_symbol = TERMINATOR;
      advance(lexer, scanner);
      checkpoint(lexer, scanner);

      while (!(lexer->eof(lexer))) {
        if (cursor(lexer) == '\n' || cursor(lexer) == ' ' || cursor(lexer) == '\r' || cursor(lexer) == '\t') {
          advance(lexer, scanner);
          continue;
        }

        if (cursor(lexer) == '|') {
          return false;
        }

        return true;
      }
    }

    if (cursor(lexer) == ';') {
      lexer->result_symbol = TERMINATOR;
      advance(lexer, scanner);
      checkpoint(lexer, scanner);
      return true;
    }

    if (lexer->eof(lexer)) {
      lexer->result_symbol = TERMINATOR;
      return true;
    }
  }

  return false;
}
