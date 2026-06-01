#include "tree_sitter/parser.h"
#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"

enum TokenType {
  EOR,
  ERROR_SENTINEL,
};

void * tree_sitter_picat_external_scanner_create() {
  return NULL;
}

void tree_sitter_picat_external_scanner_destroy(void *payload) {
}

unsigned tree_sitter_picat_external_scanner_serialize(
  void *payload,
  char *buffer
) {
  return 0;
}

void tree_sitter_picat_external_scanner_deserialize(
  void *payload,
  const char *buffer,
  unsigned length
) {
}

bool tree_sitter_picat_external_scanner_scan(
  void *payload,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  if (valid_symbols[ERROR_SENTINEL]) {
    return false;
  }
  if (valid_symbols[EOR] && lexer->lookahead == '.') {
    lexer->advance(lexer, false);
    if (lexer->eof(lexer)) {
      lexer->result_symbol = EOR;
      return true;
    } else if (lexer->lookahead == ' ' || lexer->lookahead == '\n' || lexer->lookahead == '\r') {
      // lexer->mark_end(lexer);
      lexer->advance(lexer, true);
      lexer->result_symbol = EOR;
      return true;
    }
  }
  return false;
}
