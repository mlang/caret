/**
 * RPMBash external scanner
 *
 * This scanner extends tree-sitter-bash with RPM-specific newline handling.
 *
 * THE PROBLEM:
 * In bash, newlines can be statement terminators OR insignificant whitespace
 * (when the parser is looking for more command arguments). The bash scanner
 * handles this by returning NEWLINE tokens only in certain contexts.
 *
 * When RPM statements like %global or %define appear after a bash command,
 * we need the newline between them to act as a statement terminator:
 *
 *     export FOO="bar"     <- bash command
 *     %global name value   <- RPM statement (must be separate)
 *
 * Without intervention, the bash scanner might skip the newline while looking
 * for more arguments to 'export', causing %global to be parsed as part of the
 * previous command.
 *
 * THE SOLUTION:
 * We peek ahead when we see a newline. If the next non-whitespace content is
 * an RPM keyword (%global, %define, %if, etc.), we force-return a NEWLINE
 * token to terminate the previous bash command.
 *
 * KEY INSIGHT - Lexer Position vs Token Boundaries:
 * In tree-sitter scanners, lexer->advance() moves the read position forward,
 * while lexer->mark_end() sets where the token ends IF we return true.
 * When returning false, tree-sitter resets the lexer to its original position.
 *
 * CRITICAL: If we advance the lexer to peek ahead but then want to delegate
 * to the bash scanner, we CANNOT do so - the bash scanner would see the
 * already-advanced position. Instead, we must return false and let tree-sitter
 * reset the lexer, then the grammar's own rules will handle the newline.
 */

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <string.h>

/*
 * Include the bash scanner implementation.
 *
 * We rename the bash scanner functions before including to avoid symbol
 * conflicts. This allows us to wrap them with our own implementations.
 */
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wunused-value"
#endif

#define tree_sitter_bash_external_scanner_create  _bash_external_scanner_create
#define tree_sitter_bash_external_scanner_destroy _bash_external_scanner_destroy
#define tree_sitter_bash_external_scanner_serialize \
    _bash_external_scanner_serialize
#define tree_sitter_bash_external_scanner_deserialize \
    _bash_external_scanner_deserialize
#define tree_sitter_bash_external_scanner_scan _bash_external_scanner_scan

/*
 * We use a vendored copy of tree-sitter-bash's scanner for compatibility with
 * nvim-treesitter, which builds parsers without npm/node_modules.
 *
 * To update the vendored copy after `npm install`:
 *   make update-bash-scanner
 *
 * Original path (for local development with node_modules):
 *   #include "../node_modules/tree-sitter-bash/src/scanner.c"
 */
#include "third_party/bash_scanner.c"

#undef tree_sitter_bash_external_scanner_create
#undef tree_sitter_bash_external_scanner_destroy
#undef tree_sitter_bash_external_scanner_serialize
#undef tree_sitter_bash_external_scanner_deserialize
#undef tree_sitter_bash_external_scanner_scan

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

/*
 * RPM macro names follow identifier rules: start with letter/underscore,
 * followed by letters, digits, or underscores.
 */
static inline bool is_macro_name_char(int32_t c, bool first)
{
    if (first) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/*
 * Check if a macro name represents a simple macro (statement-starting).
 *
 * Simple macros like %configure, %cmake, %make_build start new statements
 * when they appear at the beginning of a line. We require 2+ characters
 * to avoid matching printf specifiers like %s or %d.
 *
 * IMPORTANT: RPM conditionals (%if, %else, %endif, %elif, %ifarch, etc.)
 * are defined as extras in the grammar. They can appear ANYWHERE without
 * breaking parse structure - including inside multi-line commands:
 *
 *   ./configure \
 *     --prefix=/usr \
 *   %if %{with ssl}
 *     --with-ssl \
 *   %endif
 *     --disable-gzip
 *
 * Therefore, conditionals must NOT trigger NEWLINE tokens here, or they
 * would terminate the command prematurely.
 *
 * Note: Brace expansions like %{name} are handled differently - they can
 * appear within command arguments and don't start new statements.
 */
static bool is_simple_macro(const char *name, size_t len)
{
    /*
     * Any identifier with 2+ characters that starts a line is treated
     * as a statement-starting macro. This covers:
     * - Conditionals: %if, %else, %endif, %elif, %ifarch, etc.
     * - Definitions: %define, %global, %undefine
     * - Build macros: %configure, %cmake, %meson, %make_build, etc.
     * - And any other RPM macro that appears at line start
     *
     * Single-char macros like %s must use braces: %{s}
     */
    (void)name; /* Name content doesn't matter, only length */
    return len >= 2;
}

/*
 * Result codes for scan_newline_before_rpm_statement().
 *
 * We use a three-way return value because the caller needs to know:
 * 1. Whether we touched the lexer (affects whether bash scanner can be called)
 * 2. Whether we're returning a token (affects return value)
 */
typedef enum {
    /* Not at newline, or NEWLINE token not valid in this context.
     * Lexer position unchanged - safe to call bash scanner. */
    SCAN_NOT_AT_NEWLINE,

    /* Found %keyword after newline. Returning NEWLINE token.
     * Caller should return true. */
    SCAN_MATCHED_KEYWORD,

    /* At newline but no %keyword found. Lexer was advanced during peek.
     * Caller MUST return false - cannot call bash scanner with stale position.
     * Tree-sitter will reset lexer and retry. */
    SCAN_NO_KEYWORD,
} ScanNewlineResult;

/*
 * Check if we're at a newline followed by an RPM statement keyword.
 *
 * This function peeks ahead to see what follows the newline. If it's an
 * RPM keyword like %global or %if, we return a NEWLINE token to ensure
 * the previous bash command is properly terminated.
 *
 * Example where this matters:
 *
 *     ./configure --prefix=/usr
 *     %if %{with_ssl}
 *
 * Without this function, the bash scanner might treat the newline as
 * insignificant whitespace and try to parse %if as an argument to configure.
 */
static ScanNewlineResult
scan_newline_before_rpm_statement(TSLexer *lexer, const bool *valid_symbols)
{
    /* Only proceed if we're at a newline and NEWLINE is a valid token here */
    if (!valid_symbols[NEWLINE] || lexer->lookahead != '\n') {
        return SCAN_NOT_AT_NEWLINE;
    }

    /*
     * Mark the start position. If we return false later, tree-sitter will
     * reset the lexer to wherever the last mark_end was before we started.
     */
    lexer->mark_end(lexer);

    /* Consume the newline character */
    lexer->advance(lexer, false);

    /*
     * Mark the token end position HERE, right after the newline.
     *
     * This is important: if we find a keyword and return NEWLINE, the token
     * should only contain the newline character(s), NOT the %keyword that
     * follows. The %keyword will be parsed by the grammar as a separate node.
     *
     * We mark now before peeking further, so the token boundary is correct.
     */
    lexer->mark_end(lexer);

    /*
     * Skip any whitespace between the newline and potential %keyword.
     * This includes additional blank lines - we still want to catch:
     *
     *     export FOO=bar
     *
     *     %global name value
     */
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
           lexer->lookahead == '\n') {
        lexer->advance(lexer, false);
    }

    /* Not at '%' - no RPM keyword here */
    if (lexer->lookahead != '%') {
        return SCAN_NO_KEYWORD;
    }

    /* Skip past the '%' to read the keyword name */
    lexer->advance(lexer, false);

    /* Read the potential keyword into a buffer */
    char name_buf[16];
    size_t name_len = 0;

    while (is_macro_name_char(lexer->lookahead, name_len == 0) &&
           name_len < sizeof(name_buf) - 1) {
        name_buf[name_len++] = (char)lexer->lookahead;
        lexer->advance(lexer, false);
    }
    name_buf[name_len] = '\0';

    /* Check if this is a simple macro (2+ chars) that starts a statement */
    if (is_simple_macro(name_buf, name_len)) {
        /*
         * Found a simple macro like %configure, %if, %global, etc.
         * Return the NEWLINE token to terminate the previous command.
         *
         * The token end was already marked after the newline (above),
         * so the NEWLINE token contains just the newline, and the
         * %macro remains for the grammar to parse.
         */
        lexer->result_symbol = NEWLINE;
        return SCAN_MATCHED_KEYWORD;
    }

    /*
     * Single-char macro like %s - these are typically printf specifiers,
     * not statement-starting macros. Return SCAN_NO_KEYWORD so the caller
     * knows we advanced the lexer and must NOT call the bash scanner.
     */
    return SCAN_NO_KEYWORD;
}

/*
 * Scanner lifecycle functions - delegate to the wrapped bash scanner.
 */
void *tree_sitter_rpmbash_external_scanner_create(void)
{
    return _bash_external_scanner_create();
}

void tree_sitter_rpmbash_external_scanner_destroy(void *payload)
{
    _bash_external_scanner_destroy(payload);
}

unsigned tree_sitter_rpmbash_external_scanner_serialize(void *payload,
                                                        char *buffer)
{
    return _bash_external_scanner_serialize(payload, buffer);
}

void tree_sitter_rpmbash_external_scanner_deserialize(void *payload,
                                                      const char *buffer,
                                                      unsigned length)
{
    _bash_external_scanner_deserialize(payload, buffer, length);
}

/*
 * Main scanner entry point.
 *
 * We first check for the newline-before-RPM-keyword case. If that doesn't
 * apply, we delegate to the bash scanner for normal token handling.
 */
bool tree_sitter_rpmbash_external_scanner_scan(void *payload,
                                               TSLexer *lexer,
                                               const bool *valid_symbols)
{
    ScanNewlineResult result =
        scan_newline_before_rpm_statement(lexer, valid_symbols);

    switch (result) {
    case SCAN_MATCHED_KEYWORD:
        /* Found %keyword - we set up the NEWLINE token, return success */
        return true;

    case SCAN_NO_KEYWORD:
        /*
         * We peeked ahead but didn't find a keyword. The lexer position
         * has been advanced past the newline and possibly more content.
         *
         * We CANNOT call the bash scanner here - it would see the wrong
         * position and produce incorrect results.
         *
         * Instead, return false. Tree-sitter will reset the lexer to its
         * original position (before we started) and try other parse paths.
         * The grammar's extras (which include whitespace) will handle the
         * newline appropriately.
         */
        return false;

    case SCAN_NOT_AT_NEWLINE:
        /* We didn't touch the lexer - safe to delegate to bash scanner */
        break;
    }

    /* Normal case: let the bash scanner handle this token */
    return _bash_external_scanner_scan(payload, lexer, valid_symbols);
}
