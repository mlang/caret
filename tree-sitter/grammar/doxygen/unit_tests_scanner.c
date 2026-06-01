#include "tree_sitter/parser.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* HACK: We just include the scanner code */
#include "scanner.c"

/**
 * @name    Mockup TSLexer Implementation
 * @{
 */
/**
 * @brief   Special value to mark an invalid symbol
 */
#define SYMBOL_INVALID ((TSSymbol) -1)
/**
 * @brief   Mockup TSLexer type
 */
typedef struct {
    TSLexer base;           ///< Inherit from TSLexer
    const char *input;      ///< Mock input
    size_t pos;             ///< Position within input
    size_t column;          ///< Current column
    ssize_t marked_end;     ///< Marked end of token
    char marks[32];         ///< 'x' for consumed, ' ' for skipped
} MockTSLexer;

static void _mock_advance(TSLexer *_lexer, bool skip)
{
    MockTSLexer *lexer = (MockTSLexer *)_lexer;

    if (lexer->input[lexer->pos] == '\0') {
        return;
    }

    if (lexer->input[lexer->pos] == '\n') {
        lexer->column = 0;
    } else {
        lexer->column++;
    }

    lexer->marks[lexer->pos] = skip ? ' ' : 'x';

    lexer->pos++;
    _lexer->lookahead = lexer->input[lexer->pos];
}

static uint32_t _mock_get_column(TSLexer *_lexer)
{
    MockTSLexer *lexer = (MockTSLexer *)_lexer;
    return lexer->column;
}

static bool _mock_eof(const TSLexer *_lexer)
{
    MockTSLexer *lexer = (MockTSLexer *)_lexer;
    return lexer->input[lexer->pos] == '\0';
}

static void _mock_mark_end(TSLexer *_lexer)
{
    MockTSLexer *lexer = (MockTSLexer *)_lexer;
    lexer->marked_end = lexer->pos;
}

/**
 * @brief   Initialize the given TSLexer
 * @param[out]  lexer           The tree-sitter lexer to initialize
 * @param[in]   input           The test text
 * @param[in]   initial_column  The column to start at
 * @pre         @p _lexer actually is a MockTSLexer
 */
static void test_lexer_init(MockTSLexer *lexer, const char *input, size_t initial_column)
{
    *lexer = (MockTSLexer){
        .base = {
            .advance = _mock_advance,
            .get_column = _mock_get_column,
            .eof = _mock_eof,
            .mark_end = _mock_mark_end,
            .lookahead = input[initial_column],
            .result_symbol = SYMBOL_INVALID,
        },
        .input = input,
        .column = initial_column,
        .pos = initial_column,
        .marked_end = -1,
    };

    for (size_t i = 0; i < initial_column; i++) {
        lexer->marks[i] = ' ';
    }
}
/** @} */

/**
 * @name    Impromptu test "framework"
 * @{
 */
/**
 * @brief   Backbone of the @ref TEST_ASSERT macro
 *
 * @param[in]   line        Line number of the failed test
 * @param[in]   expr        The failed expression
 */
static bool _assert_fail(unsigned line, const char *expr)
{
    printf("FAIL: %s:%u: %s\n", __FILE__, line, expr);
    return false;
}

/**
 * @brief   Backbone of the @ref TEST_ASSERT_EQ macro
 *
 * @param[in]   line        Line number of the failed test
 * @param[in]   x        The failed expression
 */
static bool _assert_eq(unsigned line, const char *x_str, int64_t x,
                       const char *y_str, int64_t y)
{
    if (x == y) return true;
    
    printf("FAIL: %s:%u: %s != %s (%" PRId64 " != %" PRId64 ")\n",
            __FILE__, line, x_str, y_str, x, y);
    return false;
}

/**
 * @brief   Backbone of the @ref QUOTE macro
 * @param[in]   x           Expression to quote
 */
#define _QUOTE(x) #x
/**
 * @brief   Quote the given expression
 * @param[in]   x           Expression to quote
 */
#define QUOTE(x) _QUOTE(x)
/**
 * @brief   If @p x evaluates to `false`, print a failure and return `false`
 * @param[in]   x           Expression that should be true
 */
#define TEST_ASSERT(x) if (!(x)) return _assert_fail(__LINE__, QUOTE(x))
#define TEST_ASSERT_EQ(x, y) \
    if (!_assert_eq(__LINE__, QUOTE(x), (int64_t)(x), QUOTE(y), (int64_t)(y))) \
        return false

/**
 * @brief   Run the given test suite
 * @param[in]   x           Test function to run
 *
 * @warning     There needs to be a variable `total` and `passed` in the current
 *              scope that are updated by this macro
 */
#define RUN_TEST(x) \
    do { \
        printf("Run test %02u: %s\n", ++total, QUOTE(x)); \
        if (!x(state)) { \
            failed++; \
        } \
    } while (0)
/** @} */

/**
 * @name    The actual test cases
 * @{
 */
const bool all_symbols_valid[] = {
    true,  true,  true,  true,  true,  true,  true,  true,  true
};
const bool all_symbols_but_cont_valid[] = {
    true,  false, true,  true,  true,  true,  true,  true,  true
};
const bool all_symbols_but_new_valid[] = {
    false, true,  true,  true,  true,  true,  true,  true,  true
};
const bool code_symbols_valid[] = {
    false, false, false, false, true,  true,  true,  true,  true
};
const bool code_symbols_but_line_valid[] = {
    false, false, false, false, true,  true,  true,  false, true
};
const bool code_symbols_but_line_and_lang_end_valid[] = {
    false, false, false, false, true,  true,  false, false, true
};
const bool code_line_code_end_valid[] = {
    false, false, false, false, false, false, false, true, true
};

/**
 * @brief   Parameters describing a test to run with @ref _test_help
 */
struct test_params {
    Scanner state_in;           ///< initial state of the scanner
    const char *input;          ///< text to scan
    Scanner state_expected;     ///< expected state after scanning
    size_t token_end;           ///< expected end of the token (bytes after `input`)
    enum TokenType symbol;      ///< expected symbol
    const char *marks;          ///< positions to be accepted by the scanner
    size_t column;              ///< initial column position
    const bool *valid_symbols;        ///< symbols valid at input state
};

/**
 * @brief   Perform a scan with the given state and input and compare the
 *          result with the expectation
 * @param[in,out]   state       The internal state as allocated by the scanner
 * @param[in]       params      The test parameters
 */
static bool _test_helper(void *state, const struct test_params *params)
{
    MockTSLexer _lexer;
    TSLexer *lexer = &_lexer.base;
    Scanner state_out;
    memset(&state_out, 0x55, sizeof(state_out));
    char state_buf[2];
    bool valid_symbols[sizeof(all_symbols_valid) / sizeof(all_symbols_valid[0])];
    memcpy(valid_symbols, params->valid_symbols, sizeof(valid_symbols));
    TEST_ASSERT(sizeof(state_buf) == tree_sitter_doxygen_external_scanner_serialize((void *)&params->state_in, state_buf));
    tree_sitter_doxygen_external_scanner_deserialize(state, state_buf, sizeof(state_buf));
    test_lexer_init(&_lexer, params->input, params->column);
    size_t actual_end = 0;

    // first test that the scanner does not consume a token when that symbol
    // is not considered as valid by the grammar
    valid_symbols[params->symbol] = false;

    bool retval = tree_sitter_doxygen_external_scanner_scan(state, lexer, valid_symbols);
    TEST_ASSERT_EQ(SYMBOL_INVALID, lexer->result_symbol);
    TEST_ASSERT(!retval);
    // not accepting a symbol *MUST NOT* change the internal state
    char state_buf2[sizeof(state_buf)];
    TEST_ASSERT(sizeof(state_buf2) == tree_sitter_doxygen_external_scanner_serialize(state, state_buf2));
    TEST_ASSERT_EQ(state_buf[0], state_buf2[0]);
    TEST_ASSERT_EQ(state_buf[1], state_buf2[1]);

    // now test that we actually get the symbol when allowed
    valid_symbols[params->symbol] = true;
    test_lexer_init(&_lexer, params->input, params->column);
    TEST_ASSERT(tree_sitter_doxygen_external_scanner_scan(state, lexer, valid_symbols));
    TEST_ASSERT(sizeof(state_buf) == tree_sitter_doxygen_external_scanner_serialize(state, state_buf));
    tree_sitter_doxygen_external_scanner_deserialize(&state_out, state_buf, sizeof(state_buf));
    actual_end = (_lexer.marked_end != -1) ? (size_t)_lexer.marked_end : _lexer.pos;

    TEST_ASSERT_EQ(params->state_expected.mode, state_out.mode);
    TEST_ASSERT_EQ(params->state_expected.after_mode, state_out.after_mode);
    TEST_ASSERT_EQ(params->token_end, actual_end);
    TEST_ASSERT_EQ(params->symbol, lexer->result_symbol);

    if (params->marks) {
        bool marks_match = (0 == memcmp(_lexer.marks, params->marks, params->token_end));
        if (!marks_match) {
            printf("Input         : ");
            for (const char *i = params->input; *i != '\0'; i++) {
                switch (*i) {
                case '\n':
                    printf("␤");
                    break;
                case '\r':
                    printf("␍");
                    break;
                case '\t':
                    printf("␉");
                    break;
                default:
                    printf("%c", *i);
                }
            }
            puts("");
            printf("Marks expected: %s\n", params->marks);
            printf("Marks got     : %s\n", _lexer.marks);
            TEST_ASSERT(marks_match);
        }
    }

    return true;
}

static bool test_uncommittet_detect_c(void *state)
{
    const struct test_params params = {
        .input = "/**   a",
        .marks = "xxx ",
        .token_end = strlen("/** "),
        .state_in = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_C },
        .symbol = DOC_NEW,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_uncommittet_detect_c_after(void *state)
{
    const struct test_params params = {
        .input = "/**<   a",
        .marks = "xxxx ",
        .token_end = strlen("/**< "),
        .state_in = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .state_expected = { .after_mode = true, .mode = MODE_COMMENT_C },
        .symbol = DOC_AFTER,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_uncommittet_detect_c_skip_empty(void *state)
{
    const struct test_params params = {
        .input = "/**\n *  a",
        .marks = "xxx  x ",
        .token_end = strlen("/**\n * "),
        .state_in = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_C },
        .symbol = DOC_NEW,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_c_detect_cont(void *state)
{
    const struct test_params params = {
        .input = " *  a",
        .marks = " x ",
        .token_end = strlen(" * "),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_C },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_C },
        .symbol = DOC_CONT,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_c_detect_cont_no_strip(void *state)
{
    const struct test_params params = {
        .input = "   a",
        .marks = "",
        .token_end = strlen(""),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_C },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_C },
        .symbol = DOC_CONT,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_c_detect_cont_instead_of_new(void *state)
{
    const struct test_params params = {
        .input = " *\n *\n",
        .marks = " x",
        .token_end = strlen(" *"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_C },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_C },
        .symbol = DOC_CONT,
        .valid_symbols = all_symbols_but_new_valid,
    };

    return _test_helper(state, &params);
}

static bool test_c_detect_new(void *state)
{
    const struct test_params params = {
        .input = " * \r\n *\n *   a",
        .marks = " x    x  x ",
        .token_end = strlen(" * \r\n *\n * "),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_C },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_C },
        .symbol = DOC_NEW,
        .valid_symbols = all_symbols_but_cont_valid,
    };

    return _test_helper(state, &params);
}

static bool test_c_detect_comment_end(void *state)
{
    const struct test_params params = {
        .input = " */\r\n\n",
        .marks = " xx",
        .token_end = strlen(" */"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_C },
        .state_expected = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .symbol = DOC_END,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_uncommitted_detect_cxx(void *state)
{
    const struct test_params params = {
        .input = "/// \ta",
        .marks = "xxx ",
        .token_end = strlen("/// "),
        .state_in = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .symbol = DOC_NEW,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_uncommitted_detect_cxx_after(void *state)
{
    const struct test_params params = {
        .input = "///<\t a",
        .marks = "xxxx ",
        .token_end = strlen("///<\t"),
        .state_in = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .state_expected = { .after_mode = true, .mode = MODE_COMMENT_CXX },
        .symbol = DOC_AFTER,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_uncommitted_detect_cxx_skip_empty(void *state)
{
    const struct test_params params = {
        .input = "///\t  \t\r\n/// a",
        .marks = "xxx      xxx ",
        .token_end = strlen("///\t  \t\r\n/// "),
        .state_in = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .symbol = DOC_NEW,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_cxx_detect_cont(void *state)
{
    const struct test_params params = {
        .input = "/// foo",
        .marks = "xxx ",
        .token_end = strlen("/// "),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .symbol = DOC_CONT,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_cxx_detect_cont_instead_of_new(void *state)
{
    const struct test_params params = {
        .input = "///\n/// foo",
        .marks = "xxx",
        .token_end = strlen("///"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .symbol = DOC_CONT,
        .valid_symbols = all_symbols_but_new_valid,
    };

    return _test_helper(state, &params);
}

static bool test_cxx_detect_new(void *state)
{
    const struct test_params params = {
        .input = "///\n/// foo",
        .marks = "xxx xxx ",
        .token_end = strlen("///\n/// "),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .symbol = DOC_NEW,
        .valid_symbols = all_symbols_but_cont_valid,
    };

    return _test_helper(state, &params);
}

static bool test_cxx_skip_commit_special(void *state)
{
    const struct test_params params = {
        .input = "//! foo",
        .marks = "xxx ",
        .token_end = strlen("//! "),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .symbol = DOC_CONT,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_cxx_skip_commit_with_after_marker(void *state)
{
    const struct test_params params = {
        .input = "//!< foo",
        .marks = "xxxx ",
        .token_end = strlen("//!< "),
        .state_in = { .after_mode = true, .mode = MODE_COMMENT_CXX },
        .state_expected = { .after_mode = true, .mode = MODE_COMMENT_CXX },
        .symbol = DOC_CONT,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_cxx_skip_commit_without_after_marker(void *state)
{
    const struct test_params params = {
        .input = "//!< foo",
        .marks = "xxx",
        .token_end = strlen("//!"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .symbol = DOC_CONT,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_cxx_detect_comment_end_with_comment_and_nl(void *state)
{
    const struct test_params params = {
        .input = "///\n",
        .marks = "xxx ",
        .token_end = strlen("///\n"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .state_expected = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .symbol = DOC_END,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_cxx_detect_comment_end_with_nl(void *state)
{
    const struct test_params params = {
        .input = "\n",
        .marks = "",
        .token_end = strlen(""),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .state_expected = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .symbol = DOC_END,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_cxx_detect_comment_end_no_nl(void *state)
{
    const struct test_params params = {
        .input = "",
        .marks = "",
        .token_end = strlen(""),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_CXX },
        .state_expected = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .symbol = DOC_END,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_uncommitted_detect_markdown(void *state)
{
    const struct test_params params = {
        .input = "abc",
        .marks = "",
        .token_end = strlen(""),
        .state_in = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE },
        .symbol = DOC_NEW,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_markdown_detect_cont(void *state)
{
    const struct test_params params = {
        .input = "abc",
        .marks = "",
        .token_end = strlen(""),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE },
        .symbol = DOC_CONT,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_markdown_detect_cont_instead_of_new(void *state)
{
    const struct test_params params = {
        .input = "\nabc\n",
        .marks = "",
        .token_end = strlen(""),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE },
        .symbol = DOC_CONT,
        .valid_symbols = all_symbols_but_new_valid,
    };

    return _test_helper(state, &params);
}

static bool test_markdown_detect_new(void *state)
{
    const struct test_params params = {
        .input = "\nabc",
        .marks = " ",
        .token_end = strlen("\n"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE },
        .symbol = DOC_NEW,
        .valid_symbols = all_symbols_but_cont_valid,
    };

    return _test_helper(state, &params);
}

static bool test_markdown_detect_end_with_nl(void *state)
{
    const struct test_params params = {
        .input = "\n",
        .marks = " ",
        .token_end = strlen("\n"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE },
        .state_expected = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .symbol = DOC_END,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_markdown_detect_end_without_nl(void *state)
{
    const struct test_params params = {
        .input = "",
        .marks = "",
        .token_end = strlen(""),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE },
        .state_expected = { .after_mode = false, .mode = MODE_UNCOMMITTED },
        .symbol = DOC_END,
        .valid_symbols = all_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_start_markdown(void *state)
{
    const struct test_params params = {
        .input = " * ``````c",
        .marks = "   xxxxxx",
        .column = strlen(" * "),
        .token_end = strlen(" * ``````"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_C },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_C,
                            .code = CBMODE_MARKDOWN,
                            .fence_len = strlen("``````") },
        .symbol = CODE_START,
        .valid_symbols = code_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_start_legacy(void *state)
{
    const struct test_params params = {
        .input = "~~~{.c}",
        .marks = "xxx",
        .token_end = strlen("~~~"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_LEGACY,
                            .fence_len = strlen("~~~") },
        .symbol = CODE_START,
        .valid_symbols = code_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_start_command(void *state)
{
    const struct test_params params = {
        .input = " * @code{.c}",
        .marks = "   xxxxx",
        .column = strlen(" * "),
        .token_end = strlen(" * @code"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_C },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_C,
                            .code = CBMODE_COMMAND },
        .symbol = CODE_START,
        .valid_symbols = code_symbols_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_lang_markdown(void *state)
{
    const struct test_params params = {
        .input = " * ``````c",
        .marks = "         x",
        .column = strlen(" * ``````"),
        .token_end = strlen(" * ``````c"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_C,
                      .code = CBMODE_MARKDOWN, .fence_len = strlen("``````") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_C,
                            .code = CBMODE_MARKDOWN,
                            .fence_len = strlen("``````") },
        .symbol = CODE_LANG,
        .valid_symbols = code_symbols_but_line_and_lang_end_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_lang_legacy(void *state)
{
    const struct test_params params = {
        .input = "~~~{.c}",
        .marks = "     x",
        .column = strlen("~~~"),
        .token_end = strlen("~~~{.c"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_LEGACY, .fence_len = strlen("``````") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_LEGACY,
                            .fence_len = strlen("~~~") },
        .symbol = CODE_LANG,
        .valid_symbols = code_symbols_but_line_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_lang_command(void *state)
{
    const struct test_params params = {
        .input = "@code{.c}",
        .marks = "       x",
        .column = strlen("@code"),
        .token_end = strlen("@code{.c"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_COMMAND },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_LEGACY },
        .symbol = CODE_LANG,
        .valid_symbols = code_symbols_but_line_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_lang_end_markdown(void *state)
{
    const struct test_params params = {
        .input = "```c\n",
        .marks = "    ",
        .column = strlen("```c"),
        .token_end = strlen("```c"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_MARKDOWN, .fence_len = strlen("```") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_MARKDOWN,
                            .fence_len = strlen("```") },
        .symbol = CODE_LANG_END,
        .valid_symbols = code_symbols_but_line_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_lang_end_legacy(void *state)
{
    const struct test_params params = {
        .input = "~~~{.c}\n",
        .marks = "      x",
        .column = strlen("~~~{.c"),
        .token_end = strlen("~~~{.c}"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_LEGACY, .fence_len = strlen("~~~") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_LEGACY,
                            .fence_len = strlen("~~~") },
        .symbol = CODE_LANG_END,
        .valid_symbols = code_symbols_but_line_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_lang_end_command(void *state)
{
    const struct test_params params = {
        .input = "@code{.c}\n",
        .marks = "        x",
        .column = strlen("@code{.c"),
        .token_end = strlen("@code{.c}"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_COMMAND, .fence_len = strlen("~~~") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_LEGACY,
                            .fence_len = strlen("~~~") },
        .symbol = CODE_LANG_END,
        .valid_symbols = code_symbols_but_line_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_line_markdown(void *state)
{
    const struct test_params params = {
        .input = "int main(void)\r\n",
        .marks = "xxxxxxxxxxxxxxxx",
        .token_end = strlen("int main(void)\r\n"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_MARKDOWN, .fence_len = strlen("```") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_MARKDOWN,
                            .fence_len = strlen("```") },
        .symbol = CODE_LINE,
        .valid_symbols = code_line_code_end_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_line_legacy(void *state)
{
    const struct test_params params = {
        .input = "int main(void)\r\n",
        .marks = "xxxxxxxxxxxxxxxx",
        .token_end = strlen("int main(void)\r\n"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_LEGACY, .fence_len = strlen("```") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_LEGACY,
                            .fence_len = strlen("```") },
        .symbol = CODE_LINE,
        .valid_symbols = code_line_code_end_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_line_command(void *state)
{
    const struct test_params params = {
        .input = "int main(void)\r\n",
        .marks = "xxxxxxxxxxxxxxxx",
        .token_end = strlen("int main(void)\r\n"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_COMMAND },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_COMMAND },
        .symbol = CODE_LINE,
        .valid_symbols = code_line_code_end_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_line_markdown_tricky(void *state)
{
    const struct test_params params = {
        .input = "````\r\n",
        .marks = "xxxxxx",
        .token_end = strlen("````\r\n"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_MARKDOWN, .fence_len = strlen("`````") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_MARKDOWN,
                            .fence_len = strlen("`````") },
        .symbol = CODE_LINE,
        .valid_symbols = code_line_code_end_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_line_legacy_tricky(void *state)
{
    const struct test_params params = {
        .input = "~~~~\r\n",
        .marks = "xxxxxx",
        .token_end = strlen("~~~~\r\n"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_LEGACY, .fence_len = strlen("~~~~~") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_LEGACY,
                            .fence_len = strlen("~~~~~") },
        .symbol = CODE_LINE,
        .valid_symbols = code_line_code_end_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_line_commend_tricky(void *state)
{
    const struct test_params params = {
        .input = "@endcod3\r\n",
        .marks = "xxxxxxxxxx",
        .token_end = strlen("@endcod3\r\n"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_COMMAND },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_COMMAND },
        .symbol = CODE_LINE,
        .valid_symbols = code_line_code_end_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_line_empty_line(void *state)
{
    const struct test_params params = {
        .input = "\n\n",
        .marks = "x",
        .token_end = strlen("\n"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_COMMAND },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_COMMAND },
        .symbol = CODE_LINE,
        .valid_symbols = code_line_code_end_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_end_markdown(void *state)
{
    const struct test_params params = {
        .input = "`````\r\n",
        .marks = "xxxxx",
        .token_end = strlen("`````"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_MARKDOWN, .fence_len = strlen("`````") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_MARKDOWN,
                            .fence_len = strlen("`````") },
        .symbol = CODE_END,
        .valid_symbols = code_line_code_end_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_end_legacy(void *state)
{
    const struct test_params params = {
        .input = "~~~~~\r\n",
        .marks = "xxxxx",
        .token_end = strlen("~~~~~"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_LEGACY, .fence_len = strlen("~~~~~") },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_LEGACY,
                            .fence_len = strlen("~~~~~") },
        .symbol = CODE_END,
        .valid_symbols = code_line_code_end_valid,
    };

    return _test_helper(state, &params);
}

static bool test_detect_code_end_command(void *state)
{
    const struct test_params params = {
        .input = "@endcode\r\n",
        .marks = "xxxxxxxx",
        .token_end = strlen("@endcode"),
        .state_in = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                      .code = CBMODE_COMMAND },
        .state_expected = { .after_mode = false, .mode = MODE_COMMENT_NONE,
                            .code = CBMODE_COMMAND },
        .symbol = CODE_END,
        .valid_symbols = code_line_code_end_valid,
    };

    return _test_helper(state, &params);
}
/** @} */

int main(void)
{
    unsigned failed = 0;
    unsigned total = 0;
    void *state = tree_sitter_doxygen_external_scanner_create();

    RUN_TEST(test_uncommittet_detect_c);
    RUN_TEST(test_uncommittet_detect_c_after);
    RUN_TEST(test_uncommittet_detect_c_skip_empty);
    RUN_TEST(test_c_detect_cont);
    RUN_TEST(test_c_detect_cont_no_strip);
    RUN_TEST(test_c_detect_cont_instead_of_new);
    RUN_TEST(test_c_detect_new);
    RUN_TEST(test_c_detect_comment_end);
    RUN_TEST(test_uncommitted_detect_cxx);
    RUN_TEST(test_uncommitted_detect_cxx_after);
    RUN_TEST(test_uncommitted_detect_cxx_skip_empty);
    RUN_TEST(test_cxx_detect_cont);
    RUN_TEST(test_cxx_detect_cont_instead_of_new);
    RUN_TEST(test_cxx_detect_new);
    RUN_TEST(test_cxx_skip_commit_special);
    RUN_TEST(test_cxx_skip_commit_with_after_marker);
    RUN_TEST(test_cxx_skip_commit_without_after_marker);
    RUN_TEST(test_cxx_detect_comment_end_with_comment_and_nl);
    RUN_TEST(test_cxx_detect_comment_end_with_nl);
    RUN_TEST(test_cxx_detect_comment_end_no_nl);
    RUN_TEST(test_uncommitted_detect_markdown);
    RUN_TEST(test_markdown_detect_cont);
    RUN_TEST(test_markdown_detect_cont_instead_of_new);
    RUN_TEST(test_markdown_detect_new);
    RUN_TEST(test_markdown_detect_end_with_nl);
    RUN_TEST(test_markdown_detect_end_without_nl);
    RUN_TEST(test_detect_code_start_markdown);
    RUN_TEST(test_detect_code_start_legacy);
    RUN_TEST(test_detect_code_start_command);
    RUN_TEST(test_detect_code_lang_markdown);
    RUN_TEST(test_detect_code_lang_legacy);
    RUN_TEST(test_detect_code_lang_command);
    RUN_TEST(test_detect_code_lang_end_markdown);
    RUN_TEST(test_detect_code_lang_end_legacy);
    RUN_TEST(test_detect_code_lang_end_command);
    RUN_TEST(test_detect_code_line_markdown);
    RUN_TEST(test_detect_code_line_legacy);
    RUN_TEST(test_detect_code_line_command);
    RUN_TEST(test_detect_code_line_markdown_tricky);
    RUN_TEST(test_detect_code_line_legacy_tricky);
    RUN_TEST(test_detect_code_line_commend_tricky);
    RUN_TEST(test_detect_code_line_empty_line);
    RUN_TEST(test_detect_code_end_markdown);
    RUN_TEST(test_detect_code_end_legacy);
    RUN_TEST(test_detect_code_end_command);

    tree_sitter_doxygen_external_scanner_destroy(state);

    if (!failed) {
        printf("\nSUCCESS: All %u tests passed\n", total);
        return EXIT_SUCCESS;
    }

    printf("\nFAILURE: %u out of %u failed\n", failed, total);

    return EXIT_FAILURE;
}
