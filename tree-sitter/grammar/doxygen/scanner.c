/* SPDX-License-Identifier: 0BSD */

#include <assert.h>
#include <stdbool.h>

#include "tree_sitter/parser.h"
#include "tree_sitter/alloc.h"

/**
 * @defgroup tree_sitter_doxygen_scanner Scanner for the Doxygen tree-sitter grammar
 *
 * # Design Goal of the Scanner
 *
 * The goal of the scanner is to free the grammar from parsing the comment
 * structure, namely:
 *
 * 1. Correct and consistent stripping of C/C++ style comments, if any
 * 2. State tracking:
 *   - Are we at the start of the new block, or a continuation of of the
 *     previous one?
 *   - Have we reached the end of comments?
 *
 * # Design Decisions
 *
 * The scanner intentionally only supports C / C++ style comments, as use of
 * Doxygen in languages that use other comment syntax (such as Python) is rare.
 *
 * However, the scanner also supports scanning for files that do not contain
 * any comment markers. This is useful as Doxygen supports high level
 * documentation (such as the front page of the documentation) to be written
 * in dedicated documentation files. While historically some *.dox and *.doc
 * files that did also require C/C++ style comments have been used for that,
 * Doxygen now also supports parsing *.md / *.markdown files that do not require
 * any comment markers. Support for that use case is one of the motivation for
 * this alternative implementation of the Doxygen grammar.
 */

enum TokenType {
    DOC_NEW,            ///< Comment structure indicates a new documentation part
    DOC_CONT,           ///< Comment structure does **NOT** indicate a new part
    DOC_AFTER,          ///< Documentation applies to code before
    DOC_END,            ///< End of documentation
    CODE_START,         ///< Start of a fenced code block
    CODE_LANG,          ///< Language specifier of a code block
    CODE_LANG_END,      ///< Terminal of the language specifier
    CODE_LINE,          ///< A line of code
    CODE_END,           ///< End of a fenced code block
};

typedef enum {
    MODE_UNCOMMITTED,   ///< scanner still needs to commit to commit handling or against it
    MODE_COMMENT_CXX,   ///< C++ style comment markers
    MODE_COMMENT_C,     ///< C style comment markers
    MODE_COMMENT_NONE,  ///< No comment markers
} CommentMode;

typedef enum {
    CBMODE_NONE,        ///< Currently not within a code block
    CBMODE_COMMAND,     ///< Currently parsing a code block within @code and @endcode
    CBMODE_MARKDOWN,    ///< Currently parsing a code block within ``` and ```
    CBMODE_LEGACY,      ///< Currently parsing a code block within ~~~{.<ext>} and ~~~
} CodeBlockMode;

typedef struct {
    CommentMode mode;   ///< Type of comments to skip
    CodeBlockMode code; ///< Type of the code block mode to parse
    bool after_mode;    ///< Whether there has been an `<` marker at the start
                        ///< of the comment. (Those might be present in
                        ///< subsequent lines as well.)
    uint8_t fence_len;  ///< length of the code block fence
} Scanner;

/**
 * @brief   Bitmask to extract the comment mode from the serialized state
 */
#define SCANNER_SERIALIZE_COMMENT_MODE_MASK 0x03
/**
 * @brief   Position of the comment mode
 */
#define SCANNER_SERIALIZE_COMMENT_MODE_POS 0
/**
 * @brief   Bitmask to extract the code block mode from the serialized state
 */
#define SCANNER_SERIALIZE_CODE_MODE_MASK 0x0c
/**
 * @brief   Position of the code mode
 */
#define SCANNER_SERIALIZE_CODE_MODE_POS 2
/**
 * @brief   Flag to use for (de-)serialization of the after state
 */
#define SCANNER_SERIALIZE_AFTER_FLAG 0x80

static void _consume(TSLexer *lexer)
{
    lexer->advance(lexer, false);
}

static void _skip(TSLexer *lexer)
{
    lexer->advance(lexer, true);
}

static bool _is_space(const TSLexer *lexer)
{
   switch (lexer->lookahead) {
    case ' ':
    case '\t':
        return true;
    default:
        return false;
    }
}

static bool _is_wspace(const TSLexer *lexer)
{
   switch (lexer->lookahead) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
        return true;
    default:
        return false;
    }
}

static bool _is_eof(const TSLexer *lexer)
{
    // '\0' bytes are rather rare in the middle of the file, so we optimize
    // that here. In most cases, the indirect function call to eof() can be
    // safed this way
    return ((lexer->lookahead == '\0') && (lexer->eof(lexer)));
}

/**
 * @brief   Whipe the contents of @p scanner
 */
static void _scanner_clear(Scanner *scanner)
{
    *scanner = (Scanner){ .after_mode = 0 };
}

/**
 * @brief   Process C style comment structure.
 *
 * @pre     `scanner->mode == MODE_COMMENT_C`
 * @retval  true    A new line of documentation follows, any comment syntax was
 *                  skipped
 * @retval  false   No new line of documentation starts here
 *
 * @note    A `DOC_NEW` symbol indicates that a new paragraph may start, a
 *          `DOC_CONT` symbol indicates that the description of the previous
 *          line might continue (unless special commands or markdown constructs
 *          overrule).
 */
static bool _c_comments(Scanner *scanner, TSLexer *lexer,
                        const bool *valid_symbols,
                        bool new_block)
{
    bool at_line_start = lexer->get_column(lexer) == 0;
    lexer->mark_end(lexer);

    while (_is_space(lexer)) {
        _skip(lexer);
    }

    if (lexer->lookahead != '*') {
        if (!valid_symbols[DOC_CONT]) {
            return false;
        }
        lexer->result_symbol = DOC_CONT;
        return true;
    }

    _consume(lexer);

    if (lexer->lookahead == '/') {
        if (!valid_symbols[DOC_END]) {
            return false;
        }

        _consume(lexer);
        _scanner_clear(scanner);
        lexer->mark_end(lexer);

        lexer->result_symbol = DOC_END;
        return true;
    }

    if (!at_line_start) {
        return false;
    }

    if (_is_space(lexer)) {
        _skip(lexer);
    }

    lexer->mark_end(lexer);

    while (_is_space(lexer)) {
        _skip(lexer);
    }

    if (lexer->lookahead == '\r') {
        _skip(lexer);
    }

    if (lexer->lookahead == '\n') {
        if (!valid_symbols[DOC_NEW] && valid_symbols[DOC_CONT]) {
            lexer->result_symbol = DOC_CONT;
            return true;
        }
        _skip(lexer);
        return _c_comments(scanner, lexer, valid_symbols, true);
    }

    if (new_block) {
        if (new_block && !valid_symbols[DOC_NEW]) {
            return false;
        }

        lexer->result_symbol = DOC_NEW;
        return true;
    }

    if (!valid_symbols[DOC_CONT]) {
        return false;
    }

    lexer->result_symbol = DOC_CONT;
    return true;
}

/**
 * @brief   Process C++ style comment structure.
 *
 * @pre     `scanner->mode == MODE_COMMENT_CXX`
 * @retval  true    A new line of documentation follows, any comment syntax was
 *                  skipped
 * @retval  false   No new line of documentation starts here
 *
 * @note    A `DOC_NEW` symbol indicates that a new paragraph may start, a
 *          `DOC_CONT` symbol indicates that the description of the previous
 *          line might continue (unless special commands or markdown constructs
 *          overrule).
 */
static bool _cxx_comments(Scanner *scanner, TSLexer *lexer,
                          const bool *valid_symbols, bool new_block)
{
    if (valid_symbols[DOC_END]) {
        lexer->mark_end(lexer);
        if (_is_eof(lexer)) {
            _scanner_clear(scanner);
            lexer->result_symbol = DOC_END;
            return true;
        }
    }

    if (new_block && !valid_symbols[DOC_NEW]) {
        return false;
    }

    if (lexer->get_column(lexer) != 0) {
        return false;
    }

    while (_is_space(lexer)) {
        _skip(lexer);
    }

    if (lexer->lookahead != '/') {
        if (!valid_symbols[DOC_END]) {
            return false;
        }
        _scanner_clear(scanner);

        lexer->mark_end(lexer);
        lexer->result_symbol = DOC_END;
        return true;
    }

    _consume(lexer);

    if (lexer->lookahead != '/') {
        return false;
    }

    _consume(lexer);

    if ((lexer->lookahead != '/') && (lexer->lookahead != '!')) {
        return false;
    }
    _consume(lexer);

    if ((scanner->after_mode) && (lexer->lookahead == '<')) {
        _consume(lexer);
    }

    if (_is_space(lexer)) {
        _skip(lexer);
    }

    /* update to new end of symbol */
    lexer->mark_end(lexer);

    if (lexer->lookahead == '\r') {
        _skip(lexer);
    }

    if (lexer->lookahead == '\n') {
        if (!valid_symbols[DOC_NEW] && valid_symbols[DOC_CONT]) {
            lexer->result_symbol = DOC_CONT;
            return true;
        }
        _skip(lexer);
        return _cxx_comments(scanner, lexer, valid_symbols, true);
    }

    if (new_block) {
        lexer->result_symbol = DOC_NEW;
        return true;
    }

    if (!valid_symbols[DOC_CONT]) {
        return false;
    }

    lexer->result_symbol = DOC_CONT;
    return true;
}

/**
 * @brief   Process input from e.g. a markdown file, where no comment structure
 *          needs to be skipped
 *
 * @note    We still emit the same symbols and detect when a new paragraph
 *          starts, so that the grammar doesn't need to care about the format.
 */
static bool _no_comment_processing(Scanner *scanner, TSLexer *lexer,
                                   const bool *valid_symbols, bool new_block)
{
    if (_is_eof(lexer)) {
        if (!valid_symbols[DOC_END]) {
            return false;
        }
        lexer->result_symbol = DOC_END;
        lexer->mark_end(lexer);
        _scanner_clear(scanner);
        return true;
    }

    if (new_block && !valid_symbols[DOC_NEW]) {
        return false;
    }

    if (lexer->get_column(lexer) != 0) {
        return false;
    }

    lexer->mark_end(lexer);

    while (_is_space(lexer)) {
        _skip(lexer);
    }

    if (lexer->lookahead == '\r') {
        _skip(lexer);
    }

    if (lexer->lookahead == '\n') {
        if (!valid_symbols[DOC_NEW] && valid_symbols[DOC_CONT]) {
            lexer->result_symbol = DOC_CONT;
            return true;
        }
        _skip(lexer);
        return _no_comment_processing(scanner, lexer, valid_symbols, true);
    }

    if (new_block) {
        lexer->result_symbol = DOC_NEW;
        lexer->mark_end(lexer);
        return true;
    }

    if (!valid_symbols[DOC_CONT]) {
        return false;
    }

    lexer->result_symbol = DOC_CONT;
    return true;
}

/**
 * @brief   Detect whether C or C++ style comment structure is present and store
 *          that state internnaly
 *
 * @pre     `scanner->mode == MODE_UNCOMMITTED`
 * @post    `scanner->mode != MODE_UNCOMMITTED`
 * @retval  true    scanner committed to comment structure mode
 * @retval  false   valid_symbols prevented us to commit to something
 */
static bool _commit_to_mode(Scanner *scanner, TSLexer *lexer,
                            const bool *valid_symbols)
{
    if (!valid_symbols[DOC_NEW]) {
        /* This probably indicates a bug in the grammar, as when not in
         * committed state, the grammar really should expect DOC_NEW and
         * probably also DOC_AFTER */
        return false;
    }

    while (_is_wspace(lexer)) {
        _skip(lexer);
    }

    if (lexer->lookahead != '/') {
        scanner->mode = MODE_COMMENT_NONE;
        return _no_comment_processing(scanner, lexer, valid_symbols, true);
    }

    _consume(lexer);

    switch (lexer->lookahead) {
    case '*':
        _consume(lexer);
        if ((lexer->lookahead != '*') && (lexer->lookahead != '!')) {
            return false;
        }
        _consume(lexer);
        scanner->mode = MODE_COMMENT_C;
        break;
    case '/':
        _consume(lexer);
        if ((lexer->lookahead != '/') && (lexer->lookahead != '!')) {
            return false;
        }
        _consume(lexer);
        scanner->mode = MODE_COMMENT_CXX;
        break;
    default:
        return false;
    }

    if (lexer->lookahead == '<') {
        /* This clearly is a DOC_AFTER symbol. If the grammar does not accept
         * it here, we rather back out altogether than producing a wrong
         * symbol */
        if (!valid_symbols[DOC_AFTER]) {
            /* restore original mode */
            scanner->mode = MODE_UNCOMMITTED;
            return false;
        }
        _consume(lexer);
        lexer->result_symbol = DOC_AFTER;
        scanner->after_mode = true;
    } else {
        lexer->result_symbol = DOC_NEW;
    }

    if (_is_space(lexer)) {
        _skip(lexer);
    }

    /* we might need to roll back to here, but we also need to peek ahead
     * to skip possible line breaks */
    lexer->mark_end(lexer);

    while (_is_space(lexer)) {
        _skip(lexer);
    }

    if (lexer->lookahead == '\r') {
        _skip(lexer);
    }

    if (lexer->lookahead == '\n') {
        _skip(lexer);
        switch (scanner->mode) {
        default:
            break;
        case MODE_COMMENT_C:
            return _c_comments(scanner, lexer, valid_symbols, true);
        case MODE_COMMENT_CXX:
            return _cxx_comments(scanner, lexer, valid_symbols, true);
        }
    }

    return true;
}

/**
 * @brief   Try consuming a fenced code block start symbol
 * @param[out]      result      Whether a symbol was matched or not
 * @param[in,out]   scanner     The scanner context to use
 * @param[in,out]   lexer       The lexer to use
 *
 * @retval          true        Matching concluded, result in @p result
 * @retval          false       @p lexer is left untouched, other symbols may
 *                              still match
 */
static bool _scan_code_start(bool *result, Scanner *scanner, TSLexer *lexer)
{
    if ((lexer->lookahead == '~') || (lexer->lookahead == '`')) {
        unsigned fence_len = 1;
        char fence = lexer->lookahead;
        _consume(lexer);
        while (lexer->lookahead == fence) {
            _consume(lexer);
            fence_len++;
        }
        if (fence_len >= 3) {
            scanner->fence_len = (fence_len > UINT8_MAX) ? UINT8_MAX : (uint8_t)fence_len;
            scanner->code = (fence == '~') ? CBMODE_LEGACY : CBMODE_MARKDOWN;
            lexer->result_symbol = CODE_START;
            *result = true;
            return true;
        }
        // Not a fenced code block start, but also no other symbol
        *result = false;
        return true;
    }

    if ((lexer->lookahead != '@') && (lexer->lookahead != '\\')) {
        return false;
    }

    *result = false;
    _consume(lexer);
    if (lexer->lookahead != 'c') {
        return true;
    }
    _consume(lexer);
    if (lexer->lookahead != 'o') {
        return true;
    }
    _consume(lexer);
    if (lexer->lookahead != 'd') {
        return true;
    }
    _consume(lexer);
    if (lexer->lookahead != 'e') {
        return true;
    }
    _consume(lexer);
    *result = true;
    scanner->code = CBMODE_COMMAND;
    lexer->result_symbol = CODE_START;
    return true;
}

/**
 * @brief   Try consuming a language identifier
 * @param[out]      result      Whether a symbol was matched or not
 * @param[in,out]   scanner     The scanner context to use
 * @param[in,out]   lexer       The lexer to use
 *
 * @retval          true        Matching concluded, result in @p result
 * @retval          false       @p lexer is left untouched, other symbols may
 *                              still match
 */
static bool _scan_code_lang(bool *result, Scanner *scanner, TSLexer *lexer)
{
    assert(scanner->code != CBMODE_NONE);
    if (scanner->code == CBMODE_MARKDOWN) {
        if (_is_wspace(lexer) ||_is_eof(lexer)) {
            return false;
        }
        while (!_is_wspace(lexer) && !_is_eof(lexer)) {
            _consume(lexer);
        }
        lexer->result_symbol = CODE_LANG;
        *result = true;
        return true;
    }

    if (lexer->lookahead != '{') {
        return false;
    }
    _skip(lexer);

    *result = false;
    if (lexer->lookahead != '.') {
        return true;
    }
    _skip(lexer);

    while (!_is_wspace(lexer) && (lexer->lookahead != '}') && !_is_eof(lexer)) {
        _consume(lexer);
    }

    if (lexer->lookahead != '}') {
        return true;
    }

    lexer->result_symbol = CODE_LANG;
    *result = true;

    return true;
}

static bool _consume_word(TSLexer *lexer, const char *word)
{
    while (*word) {
        if (lexer->lookahead != *word) {
            return false;
        }
        _consume(lexer);
        word++;
    }

    return true;
}

/**
 * @brief   Try consuming a code line or a code end marker
 * @param[out]      result          Whether a symbol was matched or not
 * @param[in,out]   scanner         The scanner context to use
 * @param[in,out]   lexer           The lexer to use
 * @param[in]       valid_symbols   Symbols expected by the grammar
 *
 * @retval          true            Matching concluded, result in @p result
 * @retval          false           @p lexer is left untouched, other symbols
 *                                  may still match
 */
static bool _scan_code_line_or_end(bool *result, Scanner *scanner, TSLexer *lexer,
                                   const bool *valid_symbols)
{
    assert(scanner->code != CBMODE_NONE);

    if (scanner->code != CBMODE_COMMAND) {
        char fence = (scanner->code == CBMODE_MARKDOWN) ? '`' : '~';
        unsigned count = 0;
        while ((lexer->lookahead == fence) && (count < scanner->fence_len)) {
            _consume(lexer);
            count++;
        }

        if (count == scanner->fence_len) {
            if (!valid_symbols[CODE_END]) {
                *result = false;
                return true;
            }
            *result = true;
            scanner->code = CBMODE_NONE;
            scanner->fence_len = 0;
            lexer->result_symbol = CODE_END;
            return true;
        }

        goto match_code_line;
    }

    if ((lexer->lookahead != '@') && (lexer->lookahead != '\\')) {
        goto match_code_line;
    }
    _consume(lexer);

    if (!_consume_word(lexer, "endcode")) {
        goto match_code_line;
    }

    if (!valid_symbols[CODE_END]) {
        *result = false;
        return true;
    }

    *result = true;
    scanner->code = CBMODE_NONE;
    scanner->fence_len = 0;
    lexer->result_symbol = CODE_END;
    return true;

match_code_line:
    if (!valid_symbols[CODE_LINE]) {
        *result = false;
        return true;
    }

    while ((lexer->lookahead != '\n') && !_is_eof(lexer)) {
        _consume(lexer);
    }

    if (lexer->lookahead == '\n') {
        _consume(lexer);
    }

    *result = true;
    lexer->result_symbol = CODE_LINE;
    return true;
}

bool tree_sitter_doxygen_external_scanner_scan(void *payload,
                                               TSLexer *lexer,
                                               const bool *valid_symbols)
{
    Scanner *scanner = payload;

    if (scanner->mode != MODE_UNCOMMITTED) {
        if ((valid_symbols[CODE_START]) && (scanner->code == CBMODE_NONE)) {
            bool result = false;
            if (_scan_code_start(&result, scanner, lexer)) {
                return result;
            }
        }

        if ((valid_symbols[CODE_LANG]) && (scanner->code != CBMODE_NONE)) {
            bool result = false;
            if (_scan_code_lang(&result, scanner, lexer)) {
                return result;
            }
        }

        if (valid_symbols[CODE_LANG_END]) {
            switch (scanner->code) {
            case CBMODE_COMMAND:
            case CBMODE_LEGACY:
                if (lexer->lookahead == '}') {
                    _consume(lexer);
                    lexer->result_symbol = CODE_LANG_END;
                    return true;
                }
                break;
            case CBMODE_MARKDOWN:
                lexer->result_symbol = CODE_LANG_END;
                return true;
            case CBMODE_NONE:
                break;
            }
        }

        if ((valid_symbols[CODE_LINE] || valid_symbols[CODE_END]) && (scanner->code != CBMODE_NONE)) {
            bool result = false;
            if (_scan_code_line_or_end(&result, scanner, lexer, valid_symbols)) {
                return result;
            }
        }
    }

    switch (scanner->mode) {
    default:
    case MODE_UNCOMMITTED:
        return _commit_to_mode(scanner, lexer, valid_symbols);
    case MODE_COMMENT_C:
        return _c_comments(scanner, lexer, valid_symbols, false);
    case MODE_COMMENT_CXX:
        return _cxx_comments(scanner, lexer, valid_symbols, false);
    case MODE_COMMENT_NONE:
        return _no_comment_processing(scanner, lexer, valid_symbols, false);
    }
}

unsigned tree_sitter_doxygen_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = payload;
    uint8_t state = scanner->mode << SCANNER_SERIALIZE_COMMENT_MODE_POS;
    state |= scanner->code << SCANNER_SERIALIZE_CODE_MODE_POS;
    if (scanner->after_mode) {
        state |= SCANNER_SERIALIZE_AFTER_FLAG;
    }
    buffer[0] = (char)state;
    buffer[1] = (char)scanner->fence_len;
    return 2;
}

void tree_sitter_doxygen_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = payload;
    if (length == 2) {
        scanner->mode = (buffer[0] & SCANNER_SERIALIZE_COMMENT_MODE_MASK) >> SCANNER_SERIALIZE_COMMENT_MODE_POS;
        scanner->code = (buffer[0] & SCANNER_SERIALIZE_CODE_MODE_MASK) >> SCANNER_SERIALIZE_CODE_MODE_POS;
        scanner->after_mode = (buffer[0] & SCANNER_SERIALIZE_AFTER_FLAG);
        scanner->fence_len = (uint8_t)buffer[1];
    }
}

void *tree_sitter_doxygen_external_scanner_create() {
    Scanner *scanner = ts_malloc(sizeof(Scanner));
    _scanner_clear(scanner);
    return scanner;
}

void tree_sitter_doxygen_external_scanner_destroy(void *payload) {
    ts_free(payload);
}
