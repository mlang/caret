/**
 * @file scanner.c
 * @brief Tree-sitter external scanner for RPM specification files
 *
 * This file implements an external scanner for parsing RPM spec files with
 * Tree-sitter.
 *
 * It handles the lexical analysis of RPM spec macro syntax including:
 * - %{macro} - standard macro expansion
 * - %[expr] - macro expression evaluation
 * - %(shell) - shell command execution
 *
 * See also:
 * https://tree-sitter.github.io/tree-sitter/creating-parsers/4-external-scanners.html
 */

/* ========================================================================== */
/* INCLUDES AND MACROS                                                        */
/* ========================================================================== */

#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <ctype.h>
#include <string.h>

/**
 * @brief Maximum lines to scan ahead for section keywords
 *
 * This bounds the lookahead to avoid pathological cases with very large
 * conditional blocks. 2000 lines should cover most real-world specs.
 */
#define MAX_LOOKAHEAD_LINES 2000

/** @brief Get the number of elements in a static array */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/** @brief String type alias for character arrays */
typedef Array(char) String;

/* ========================================================================== */
/* TYPES AND CONSTANTS                                                        */
/* ========================================================================== */

/**
 * @brief Token types recognized by the RPM spec scanner
 *
 * These tokens represent different types of macro syntax elements
 * that can appear in RPM specification files.
 *
 * IMPORTANT: The order must match the externals array in grammar.js
 *
 * ORDERING RATIONALE: Tokens are ordered by frequency of occurrence.
 * During error recovery, tree-sitter may try tokens in order, so placing
 * the most common tokens first improves recovery behavior:
 *
 * 1. SIMPLE_MACRO (%name) - by far the most common pattern (~80% of macros)
 * 2. Other macro types - less common but still frequent
 * 3. Conditional tokens - used in control flow
 * 4. Context-specific tokens (EXPAND_CODE, SCRIPT_CODE) - rare, only valid
 *    in specific contexts like %{expand:...} or %(...)
 */
enum TokenType {
    /* Most common tokens first for better error recovery */
    SIMPLE_MACRO,          /**< Simple macro expansion: %name */
    PARAMETRIC_MACRO_NAME, /**< Macro name at line start for parametric
                              expansion */
    NEGATED_MACRO,         /**< Negated macro expansion: %!name */
    SPECIAL_MACRO,   /**< Special macro variables: %*, %**, %#, %0-9, %nil */
    ESCAPED_PERCENT, /**< Escaped percent sign: %% */
    /* Context-aware conditional tokens for distinguishing top-level vs
       scriptlet */
    TOP_LEVEL_IF,      /**< %if at top-level or containing section keywords */
    TOP_LEVEL_IFARCH,  /**< %ifarch at top-level */
    TOP_LEVEL_IFNARCH, /**< %ifnarch at top-level */
    TOP_LEVEL_IFOS,    /**< %ifos at top-level */
    TOP_LEVEL_IFNOS,   /**< %ifnos at top-level */
    /* Subsection context tokens (description, package, sourcelist, patchlist)
     */
    SUBSECTION_IF,      /**< %if inside subsection (text content) */
    SUBSECTION_IFARCH,  /**< %ifarch inside subsection */
    SUBSECTION_IFNARCH, /**< %ifnarch inside subsection */
    SUBSECTION_IFOS,    /**< %ifos inside subsection */
    SUBSECTION_IFNOS,   /**< %ifnos inside subsection */
    /* Scriptlet section context tokens */
    SCRIPTLET_IF, /**< %if inside scriptlet section without section keywords */
    SCRIPTLET_IFARCH,  /**< %ifarch inside scriptlet section */
    SCRIPTLET_IFNARCH, /**< %ifnarch inside scriptlet section */
    SCRIPTLET_IFOS,    /**< %ifos inside scriptlet section */
    SCRIPTLET_IFNOS,   /**< %ifnos inside scriptlet section */
    /* Files section context tokens */
    FILES_IF,      /**< %if inside %files section */
    FILES_IFARCH,  /**< %ifarch inside %files section */
    FILES_IFNARCH, /**< %ifnarch inside %files section */
    FILES_IFOS,    /**< %ifos inside %files section */
    FILES_IFNOS,   /**< %ifnos inside %files section */
    /* Context-specific tokens - only valid in specific macro contexts */
    EXPAND_CODE, /**< Raw text inside %{expand:...} with balanced braces */
    SCRIPT_CODE, /**< Raw text inside %(...) with balanced parentheses */
    /* Scriptlet section tokens with word boundary checking */
    SECTION_PREP,                   /**< %prep with word boundary */
    SECTION_GENERATE_BUILDREQUIRES, /**< %generate_buildrequires with word
                                       boundary */
    SECTION_CONF,                   /**< %conf with word boundary */
    SECTION_BUILD,                  /**< %build with word boundary */
    SECTION_INSTALL,                /**< %install with word boundary */
    SECTION_CHECK,                  /**< %check with word boundary */
    SECTION_CLEAN,                  /**< %clean with word boundary */
    /* Newline token for explicit line termination */
    NEWLINE /**< Newline character for line-sensitive contexts */
};

/**
 * @brief Main scanner state structure
 *
 * Contains cached lookahead results to avoid expensive repeated scans.
 * When parsing nested conditionals, we often need to scan ahead to check
 * if the block contains section keywords. Caching avoids re-scanning
 * the same content for each nested conditional.
 */
struct Scanner {
    bool lookahead_cache_valid; /**< Whether cached result is valid */
    bool lookahead_has_section; /**< Cached result: found section keyword? */
};

/**
 * @brief RPM spec keywords that should not be matched as simple macros
 *
 * These are reserved words that have special meaning in RPM specs.
 * The scanner must NOT match these as SIMPLE_MACRO tokens.
 *
 * Note: Section keywords (prep, build, install, etc.) are in SECTION_KEYWORDS.
 * The is_keyword() function checks both arrays.
 */
static const char *const KEYWORDS[] = {
    /* Conditionals */
    "if",
    "elif",
    "else",
    "endif",
    "ifarch",
    "ifnarch",
    "elifarch",
    "ifos",
    "ifnos",
    "elifos",
    /* Definitions */
    "define",
    "global",
    "undefine",
    /* Special macros handled by grammar */
    "setup",
    "autosetup",
    "patch",
    "autopatch",
    /* Builtin string macros */
    "echo",
    "error",
    "expand",
    "getenv",
    "getncpus",
    "len",
    "lower",
    "macrobody",
    "quote",
    "reverse",
    "shescape",
    "shrink",
    "upper",
    "verbose",
    "warn",
    /* Builtin path macros */
    "basename",
    "dirname",
    "exists",
    "load",
    "suffix",
    "uncompress",
    /* Builtin URL macros */
    "url2path",
    "u2p",
    /* Builtin multi-arg macros */
    "gsub",
    "sub",
    "rep",
    /* Builtin standalone macros */
    "dnl",
    "dump",
    "rpmversion",
    "trace",
    /* Other builtins */
    "expr",
    "lua",
    /* End marker */
    NULL,
};

/**
 * @brief Subsection keywords for metadata sections
 *
 * These define package metadata and don't contain shell code.
 */
static const char *const SUBSECTION_KEYWORDS[] = {
    "package",
    "description",
    "sourcelist",
    "patchlist",
    "changelog",
    /* End marker */
    NULL,
};

/**
 * @brief Section keywords that indicate top-level context
 *
 * When a %if body contains any of these keywords, it should be
 * parsed as a top-level conditional, not a scriptlet-level one.
 */
static const char *const SCRIPTLET_KEYWORDS[] = {
    /* Main sections */
    "prep",
    "generate_buildrequires",
    "conf",
    "build",
    "install",
    "check",
    "clean",
    /* Runtime scriptlets */
    "pre",
    "post",
    "preun",
    "postun",
    "pretrans",
    "posttrans",
    "preuntrans",
    "postuntrans",
    /* Triggers */
    "triggerin",
    "triggerun",
    "triggerpostun",
    "triggerprein",
    /* File triggers */
    "filetriggerin",
    "filetriggerun",
    "filetriggerpostun",
    "transfiletriggerin",
    "transfiletriggerun",
    "transfiletriggerpostun",
    /* End marker */
    NULL,
};

/**
 * @brief File directive keywords that should only be blocked in %files context
 *
 * These keywords have special meaning in %files sections but can be valid
 * macro names in shell scriptlets. Only block them as SIMPLE_MACRO when
 * we're in a %files context.
 */
static const char *const FILES_KEYWORDS[] = {
    "defattr",
    "attr",
    "config",
    "doc",
    "docdir",
    "dir",
    "license",
    "verify",
    "ghost",
    "exclude",
    "artifact",
    "missingok",
    "readme",
    /* End marker */
    NULL,
};

/* ========================================================================== */
/* HELPER FUNCTIONS                                                           */
/* ========================================================================== */

/**
 * @brief Check if character is valid identifier start (letter or underscore)
 */
static inline bool is_identifier_start(int32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/**
 * @brief Check if a character can start a valid macro after %
 *
 * Returns true if the character following % could be the start of:
 * - %% (escaped percent)
 * - %{...} (braced macro)
 * - %(...) (shell macro)
 * - %[...] (expression macro)
 * - %!name or %!?name (negated/conditional)
 * - %?name (conditional)
 * - %name (simple macro - starts with letter or underscore)
 * - %* or %** (special macro)
 * - %# (special macro)
 * - %0-%9 (positional argument)
 *
 * @param c The character following %
 * @return true if this could start a valid macro, false otherwise
 */
static inline bool is_macro_start(int32_t c)
{
    return c == '%' || c == '{' || c == '(' || c == '[' || c == '!' ||
           c == '?' || c == '*' || c == '#' || is_identifier_start(c) ||
           isdigit(c);
}

/**
 * @brief Check if character is valid identifier continuation
 */
static inline bool is_identifier_char(int32_t c)
{
    return is_identifier_start(c) || isdigit(c);
}

/**
 * @brief Check if character is horizontal whitespace (space or tab)
 *
 * Unlike isspace(), this excludes newlines and other vertical whitespace.
 * Used for same-line whitespace checks.
 */
static inline bool is_horizontal_space(int32_t c)
{
    return c == ' ' || c == '\t';
}

/**
 * @brief Check if identifier matches a literal string
 *
 * Compares a length-prefixed string against a null-terminated literal.
 * More readable than the raw (len == X && strncmp(...) == 0) pattern.
 *
 * @param literal The null-terminated string to compare against
 * @param id The identifier buffer (not null-terminated)
 * @param len Length of the identifier
 * @return true if they match, false otherwise
 */
static inline bool strequal(const char *literal, const char *id, size_t len)
{
    size_t lit_len = strlen(literal);
    return len == lit_len && strncmp(id, literal, lit_len) == 0;
}

/**
 * @brief Check if identifier is "nil" (special macro)
 */
static inline bool is_nil(const char *id, size_t len)
{
    return strequal("nil", id, len);
}

/**
 * @brief Check if identifier is legacy patch macro (patchN where N is digits)
 *
 * These are handled by the grammar's patch_legacy_token rule.
 */
static inline bool is_patch_legacy(const char *id, size_t len)
{
    if (len < 6) { /* "patch" + at least one digit */
        return false;
    }
    if (strncmp(id, "patch", 5) != 0) {
        return false;
    }
    /* Check remaining chars are all digits */
    for (size_t i = 5; i < len; i++) {
        if (id[i] < '0' || id[i] > '9') {
            return false;
        }
    }
    return true;
}

/**
 * @brief Check if a string matches any keyword in an array
 *
 * @param str The string to check
 * @param len Length of the string
 * @param keywords NULL-terminated array of keywords
 * @return true if str matches a keyword, false otherwise
 */
static bool
matches_keyword_array(const char *str, size_t len, const char *const *keywords)
{
    for (const char *const *kw = keywords; *kw != NULL; kw++) {
        size_t kw_len = strlen(*kw);
        if (len == kw_len && strncmp(str, *kw, len) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check if a string matches a scriptlet keyword
 */
static bool is_scriptlet_keyword(const char *str, size_t len)
{
    return matches_keyword_array(str, len, SCRIPTLET_KEYWORDS);
}

/**
 * @brief Check if a string matches a subsection keyword
 */
static bool is_subsection_keyword(const char *str, size_t len)
{
    return matches_keyword_array(str, len, SUBSECTION_KEYWORDS);
}

/**
 * @brief Check if a string matches any section keyword
 */
static bool is_section_keyword(const char *str, size_t len)
{
    return is_subsection_keyword(str, len) || is_scriptlet_keyword(str, len) ||
           strequal("files", str, len);
}

/**
 * @brief Check if a string matches an RPM keyword (either regular or section)
 *
 * @param str The string to check
 * @param len Length of the string
 * @return true if str is a keyword, false otherwise
 */
static bool is_keyword(const char *str, size_t len)
{
    return matches_keyword_array(str, len, KEYWORDS) ||
           matches_keyword_array(str, len, SUBSECTION_KEYWORDS) ||
           matches_keyword_array(str, len, SCRIPTLET_KEYWORDS) ||
           strequal("files", str, len);
}

/**
 * @brief Check if a string matches a files directive keyword
 */
static bool is_files_keyword(const char *str, size_t len)
{
    return matches_keyword_array(str, len, FILES_KEYWORDS);
}

/**
 * @brief Advances the lexer to the next character
 * @param lexer The Tree-sitter lexer instance
 */
static inline void advance(TSLexer *lexer)
{
    lexer->advance(lexer, false);
}

/* ========================================================================== */
/* CONTENT SCANNERS                                                           */
/* ========================================================================== */

/**
 * @brief Scans raw content inside %{expand:...} with balanced braces
 *
 * This function reads characters until it finds:
 * - The closing } at depth 0 (end of expand macro)
 * - A % character (potential macro start - let grammar handle it)
 *
 * It tracks brace nesting depth to handle content like:
 *   %{expand: return {0:0, 11:+1}[c] }
 *
 * By stopping at %, macros inside expand content will be parsed
 * by the grammar and properly highlighted.
 *
 * @param lexer The Tree-sitter lexer instance
 * @return true if content was successfully scanned, false otherwise
 */
static bool scan_expand_content(TSLexer *lexer)
{
    int32_t brace_depth = 0;
    bool has_content = false;

    while (!lexer->eof(lexer)) {
        switch (lexer->lookahead) {
        case '%':
            /* Mark position before % so we can stop here if needed */
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->eof(lexer)) {
                /* Trailing % at EOF - include it */
                lexer->mark_end(lexer);
                has_content = true;
                goto done;
            }

            switch (lexer->lookahead) {
            case '%':
            case '#':
            case '*':
                /* %%, %#, %* - consume as content (escaped or special macro) */
                /* These will be re-evaluated after expand */
                advance(lexer);
                lexer->mark_end(lexer);
                has_content = true;
                continue;
            case '{':
                /* %{ - real macro expansion, stop BEFORE the % */
                /* mark_end was called before %, so token ends there */
                goto done;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                /* %0-%9 - positional arg, consume as content */
                while (isdigit(lexer->lookahead)) {
                    advance(lexer);
                }
                lexer->mark_end(lexer);
                has_content = true;
                continue;
            default:
                /* Other % sequences - include % and continue */
                lexer->mark_end(lexer);
                has_content = true;
                continue;
            }
        case '{':
            /* Nested opening brace - track depth */
            brace_depth++;
            has_content = true;
            advance(lexer);
            lexer->mark_end(lexer);
            continue;
        case '}':
            if (brace_depth == 0) {
                /* This is the closing brace of %{expand:...} */
                /* Don't consume it - let the grammar handle it */
                goto done;
            }
            /* Closing a nested brace */
            brace_depth--;
            has_content = true;
            advance(lexer);
            lexer->mark_end(lexer);
            continue;
        default:
            /* Any other character is part of the content */
            has_content = true;
            advance(lexer);
            lexer->mark_end(lexer);
            continue;
        }
    }

done:

    /* Note: mark_end is called inline after consuming each character/sequence.
     * This ensures we don't overwrite the mark set before %{ when we break. */

    return has_content;
}

/**
 * @brief Scans raw content inside %(...) with balanced parentheses
 *
 * This function reads characters until it finds:
 * - The closing ) at depth 0 (end of shell macro)
 * - A % character (potential macro start - let grammar handle it)
 *
 * It tracks parenthesis nesting depth to handle content like:
 *   %(test $(echo hello) = hello && echo success)
 *
 * By stopping at %, macros inside shell content will be parsed
 * by the grammar and properly highlighted.
 *
 * @param lexer The Tree-sitter lexer instance
 * @return true if content was successfully scanned, false otherwise
 */
static bool scan_shell_content(TSLexer *lexer)
{
    int32_t paren_depth = 0;
    bool has_content = false;

    while (!lexer->eof(lexer)) {
        switch (lexer->lookahead) {
        case '%':
            /* Mark position before % so we can stop here if needed */
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->eof(lexer)) {
                /* Trailing % at EOF - include it as content */
                lexer->mark_end(lexer);
                has_content = true;
                goto done;
            }
            /* Check if what follows can start a valid macro */
            if (is_macro_start(lexer->lookahead)) {
                /* Real macro start - stop BEFORE the % */
                /* mark_end was called before %, so token ends there */
                goto done;
            }
            /* Not a valid macro start (e.g., %. in ${var%.*}) */
            /* Include % as shell content and continue */
            lexer->mark_end(lexer);
            has_content = true;
            continue;
        case '(':
            /* Nested opening paren - track depth */
            paren_depth++;
            has_content = true;
            advance(lexer);
            lexer->mark_end(lexer);
            continue;
        case ')':
            if (paren_depth == 0) {
                /* This is the closing paren of %(...) */
                /* Don't consume it - let the grammar handle it */
                goto done;
            }
            /* Closing a nested paren */
            paren_depth--;
            has_content = true;
            advance(lexer);
            lexer->mark_end(lexer);
            continue;
        default:
            /* Any other character is part of the content */
            has_content = true;
            advance(lexer);
            lexer->mark_end(lexer);
            continue;
        }
    }

done:
    return has_content;
}

/* ========================================================================== */
/* TOKEN SCANNERS                                                             */
/* ========================================================================== */

/** Function pointer type for keyword checking */
typedef bool (*keyword_checker_fn)(const char *, size_t);

/**
 * @brief Lookahead to check if %if body contains matching keywords
 *
 * Scans ahead until %endif, looking for keywords that match the checker.
 * Tracks conditional nesting to find the matching %endif.
 *
 * @param lexer The Tree-sitter lexer (position will be restored)
 * @param checker Function to check if a keyword matches
 * @return true if the body contains matching keywords, false otherwise
 */
static bool conditional_body_has_keyword(TSLexer *lexer,
                                         keyword_checker_fn checker)
{
    /* Track nesting depth of conditionals */
    int32_t nesting = 1; /* We're already inside one %if */
    int32_t lines_scanned = 0;
    bool at_line_start = true;

    /* Scan character by character, looking for matching keywords */
    while (!lexer->eof(lexer) && lines_scanned < MAX_LOOKAHEAD_LINES) {
        int32_t c = lexer->lookahead;

        if (c == '\r' || c == '\n') {
            /* Newline - next character is at line start */
            lexer->advance(lexer, false);
            if (c == '\r' && lexer->lookahead == '\n') {
                lexer->advance(lexer, false);
            }
            at_line_start = true;
            lines_scanned++;
            continue;
        }

        if (c == ' ' || c == '\t') {
            /* Whitespace at line start - still at line start */
            lexer->advance(lexer, false);
            continue;
        }

        if (c == '%' && at_line_start) {
            /* Potential keyword at line start */
            lexer->advance(lexer, false);

            /* Buffer the identifier */
            char id_buf[32];
            size_t id_len = 0;

            while (is_identifier_char(lexer->lookahead) &&
                   id_len < sizeof(id_buf) - 1) {
                id_buf[id_len++] = (char)lexer->lookahead;
                lexer->advance(lexer, false);
            }
            id_buf[id_len] = '\0';

            if (id_len > 0) {
                /* Check for %endif (end of conditional) */
                if (strequal("endif", id_buf, id_len)) {
                    nesting--;
                    if (nesting == 0) {
                        /* Found matching %endif - no matching keywords found */
                        return false;
                    }
                }
                /* Check for nested %if/%ifarch/%ifos */
                else if (strequal("if", id_buf, id_len) ||
                         strequal("ifarch", id_buf, id_len) ||
                         strequal("ifnarch", id_buf, id_len) ||
                         strequal("ifos", id_buf, id_len) ||
                         strequal("ifnos", id_buf, id_len)) {
                    nesting++;
                }
                /* Check using the provided checker function */
                else if (checker(id_buf, id_len)) {
                    return true;
                }
            }
            at_line_start = false;
        } else {
            /* Other character - not at line start anymore */
            at_line_start = false;
            lexer->advance(lexer, false);
        }
    }

    /* Reached EOF or max lines without finding matching keyword */
    return false;
}

/**
 * @brief Lookahead to check if %if body contains section keywords
 *
 * Used to determine if a conditional inside a scriptlet is top-level
 * (contains sections like %files, %package) or scriptlet-level.
 */
static bool conditional_body_has_section(TSLexer *lexer)
{
    return conditional_body_has_keyword(lexer, is_section_keyword);
}

/**
 * @brief Lookahead to check if %if body contains scriptlet keywords
 *
 * Used inside %files sections to determine if a conditional should exit
 * files context. %files can contain nested %files but not scriptlets.
 */
static bool conditional_body_has_scriptlet(TSLexer *lexer)
{
    return conditional_body_has_keyword(lexer, is_scriptlet_keyword);
}

/**
 * @brief Scans macro content after the % prefix
 *
 * This function handles the content after % in macro expansions.
 * The grammar is responsible for matching the % prefix, then calls
 * the scanner to match the rest:
 * - % (second %) for escaped percent - returns ESCAPED_PERCENT
 * - !name for negated macro - returns NEGATED_MACRO
 * - name for simple macro - returns SIMPLE_MACRO
 * - *, **, #, 0-9, nil for special macros - returns SPECIAL_MACRO
 *
 * @param lexer The Tree-sitter lexer instance
 * @param valid_symbols Array indicating which token types are valid
 * @return true if a macro token was matched, false otherwise
 */
static bool scan_macro(TSLexer *lexer, const bool *valid_symbols)
{
    int32_t c = lexer->lookahead;

    /* Mark potential token start */
    lexer->mark_end(lexer);

    switch (c) {
    case '%':
        /* Second % for escaped percent (%%) */
        if (valid_symbols[ESCAPED_PERCENT]) {
            advance(lexer);
            lexer->mark_end(lexer);
            lexer->result_symbol = ESCAPED_PERCENT;
            return true;
        }
        return false;

    case '!':
        /* !name for negated macro */
        if (!valid_symbols[NEGATED_MACRO]) {
            return false;
        }
        advance(lexer);
        /* Check for !? which is conditional, not negated macro */
        if (lexer->lookahead == '?') {
            return false;
        }
        /* Must be followed by identifier */
        if (!is_identifier_start(lexer->lookahead)) {
            return false;
        }
        /* Consume identifier */
        while (is_identifier_char(lexer->lookahead)) {
            advance(lexer);
        }
        lexer->mark_end(lexer);
        lexer->result_symbol = NEGATED_MACRO;
        return true;

    case '*':
        /* * or ** for special macro */
        if (!valid_symbols[SPECIAL_MACRO]) {
            return false;
        }
        advance(lexer);
        if (lexer->lookahead == '*') {
            advance(lexer); /* ** */
        }
        lexer->mark_end(lexer);
        lexer->result_symbol = SPECIAL_MACRO;
        return true;

    case '#':
        /* # for argument count */
        if (!valid_symbols[SPECIAL_MACRO]) {
            return false;
        }
        advance(lexer);
        lexer->mark_end(lexer);
        lexer->result_symbol = SPECIAL_MACRO;
        return true;

    default:
        /* Check for 0-9 (positional args) */
        if (isdigit(c)) {
            if (!valid_symbols[SPECIAL_MACRO]) {
                return false;
            }
            /* Consume all digits */
            while (isdigit(lexer->lookahead)) {
                advance(lexer);
            }
            lexer->mark_end(lexer);
            lexer->result_symbol = SPECIAL_MACRO;
            return true;
        }

        /* Check for identifier (simple macro) */
        if (is_identifier_start(c)) {
            if (!valid_symbols[SIMPLE_MACRO]) {
                return false;
            }

            /* Buffer the identifier to check for keywords */
            char id_buf[64];
            size_t id_len = 0;

            /* Consume identifier while buffering */
            while (is_identifier_char(lexer->lookahead) &&
                   id_len < sizeof(id_buf) - 1) {
                id_buf[id_len++] = (char)lexer->lookahead;
                advance(lexer);
            }
            /* Consume any remaining chars if buffer was too small */
            while (is_identifier_char(lexer->lookahead)) {
                advance(lexer);
                id_len++;
            }
            id_buf[id_len < sizeof(id_buf) ? id_len : sizeof(id_buf) - 1] =
                '\0';

            /* Check if it's a keyword - if so, don't match */
            if (is_keyword(id_buf, id_len)) {
                return false;
            }

            /* Check if it's legacy patch syntax (patchN) - let grammar handle
             */
            if (is_patch_legacy(id_buf, id_len)) {
                return false;
            }

            /* Check if it's "nil" - special macro, not simple macro */
            if (is_nil(id_buf, id_len)) {
                if (valid_symbols[SPECIAL_MACRO]) {
                    lexer->mark_end(lexer);
                    lexer->result_symbol = SPECIAL_MACRO;
                    return true;
                }
                return false;
            }

            lexer->mark_end(lexer);
            lexer->result_symbol = SIMPLE_MACRO;
            return true;
        }

        /* Not a recognized macro pattern */
        return false;
    }
}

/**
 * @brief Check for section keywords with caching
 *
 * Uses cached result if available, otherwise performs lookahead and caches.
 */
static bool conditional_body_has_section_cached(struct Scanner *scanner,
                                                TSLexer *lexer)
{
    if (scanner->lookahead_cache_valid) {
        return scanner->lookahead_has_section;
    }

    bool result = conditional_body_has_section(lexer);
    scanner->lookahead_cache_valid = true;
    scanner->lookahead_has_section = result;
    return result;
}

/**
 * @brief Context-specific tokens for a conditional keyword
 *
 * Groups the tokens and validity flags for all 4 context types.
 * This reduces the number of variables and makes the code clearer.
 */
struct CondTokens {
    enum TokenType top; /**< Top-level token */
    enum TokenType
        subsection; /**< Subsection token (description, package, etc.) */
    enum TokenType scriptlet; /**< Scriptlet token */
    enum TokenType files;     /**< Files section token */
    bool top_valid;        /**< Top-level token is valid in current context */
    bool subsection_valid; /**< Subsection token is valid */
    bool scriptlet_valid;  /**< Scriptlet token is valid */
    bool files_valid;      /**< Files token is valid */
};

/**
 * @brief Conditional keyword definition for table-driven lookup
 */
struct CondKeyword {
    const char *name;          /**< Keyword name (e.g., "if", "ifarch") */
    enum TokenType top;        /**< Top-level token for this keyword */
    enum TokenType subsection; /**< Subsection token for this keyword */
    enum TokenType scriptlet;  /**< Scriptlet token for this keyword */
    enum TokenType files;      /**< Files token for this keyword */
};

/**
 * @brief Table of conditional keywords and their tokens
 */
static const struct CondKeyword COND_KEYWORDS[] = {
    {"if", TOP_LEVEL_IF, SUBSECTION_IF, SCRIPTLET_IF, FILES_IF},
    {"ifarch",
     TOP_LEVEL_IFARCH,
     SUBSECTION_IFARCH,
     SCRIPTLET_IFARCH,
     FILES_IFARCH},
    {"ifnarch",
     TOP_LEVEL_IFNARCH,
     SUBSECTION_IFNARCH,
     SCRIPTLET_IFNARCH,
     FILES_IFNARCH},
    {"ifos", TOP_LEVEL_IFOS, SUBSECTION_IFOS, SCRIPTLET_IFOS, FILES_IFOS},
    {"ifnos", TOP_LEVEL_IFNOS, SUBSECTION_IFNOS, SCRIPTLET_IFNOS, FILES_IFNOS},
};
#define NUM_COND_KEYWORDS ARRAY_SIZE(COND_KEYWORDS)

/**
 * @brief Select which context token to emit
 *
 * Priority order:
 * 1. Files context - always wins (can handle nested sections)
 * 2. Exclusive context - only one of subsection/scriptlet/top is valid
 * 3. Ambiguous (top + other) - use lookahead to decide
 *
 * @param scanner Scanner state for caching
 * @param lexer Lexer for lookahead
 * @param ctx Context tokens and validity
 * @return The token to emit
 */
static enum TokenType
select_conditional_token_type(struct Scanner *scanner,
                              TSLexer *lexer,
                              const struct CondTokens *ctx)
{
    /*
     * Files context with top-level also valid - use lookahead
     *
     * When both files_valid and top_valid are true, we need to check
     * if the conditional body contains SCRIPTLET keywords (like %pre, %post).
     * If it does, we should use top_level_if to allow the scriptlet to
     * be parsed correctly, not files_if which would try to parse
     * %pre as a file path.
     *
     * Note: We only check for scriptlet keywords, NOT %files or subsection
     * keywords, because nested %files sections ARE valid inside files
     * conditionals (e.g., %if %{with x} ... %files subpkg ... %endif).
     */
    if (ctx->files_valid && ctx->top_valid) {
        bool has_scriptlet = conditional_body_has_scriptlet(lexer);
        if (has_scriptlet) {
            /* Body contains scriptlet - use top-level */
            return ctx->top;
        }
        /* Body only contains files content - use files context */
        return ctx->files;
    }

    /* Only files context is valid */
    if (ctx->files_valid) {
        return ctx->files;
    }

    /* Only subsection is valid */
    if (ctx->subsection_valid && !ctx->top_valid && !ctx->scriptlet_valid) {
        return ctx->subsection;
    }

    /* Only scriptlet is valid */
    if (ctx->scriptlet_valid && !ctx->top_valid && !ctx->subsection_valid) {
        scanner->lookahead_cache_valid = false;
        return ctx->scriptlet;
    }

    /* Only top-level is valid */
    if (ctx->top_valid && !ctx->subsection_valid && !ctx->scriptlet_valid) {
        scanner->lookahead_cache_valid = false;
        return ctx->top;
    }

    /* Ambiguous: top + subsection or top + scriptlet - use lookahead */
    if (ctx->top_valid && (ctx->subsection_valid || ctx->scriptlet_valid)) {
        bool has_section = conditional_body_has_section_cached(scanner, lexer);
        /* Invalidate cache for next conditional */
        scanner->lookahead_cache_valid = false;
        if (has_section) {
            /* Body contains sections - use top-level */
            return ctx->top;
        }
        /* Body doesn't contain sections - use context-specific token */
        return ctx->subsection_valid ? ctx->subsection : ctx->scriptlet;
    }

    /* Fallback */
    if (ctx->subsection_valid) {
        return ctx->subsection;
    }
    if (ctx->scriptlet_valid) {
        return ctx->scriptlet;
    }
    return ctx->top;
}

/**
 * @brief Check if any conditional token is valid in the current context
 *
 * @param valid_symbols Array indicating which token types are valid
 * @return true if at least one conditional token is valid
 */
static bool any_conditional_valid(const bool *valid_symbols)
{
    for (size_t i = 0; i < NUM_COND_KEYWORDS; i++) {
        const struct CondKeyword *kw = &COND_KEYWORDS[i];
        if (valid_symbols[kw->top] || valid_symbols[kw->subsection] ||
            valid_symbols[kw->scriptlet] || valid_symbols[kw->files]) {
            return true;
        }
    }
    return false;
}

/* ========================================================================== */
/* SCRIPTLET SECTION SCAN LOGIC                                               */
/* ========================================================================== */

/**
 * @brief Check if any scriptlet section token is valid
 */
static inline bool any_section_token_valid(const bool *valid_symbols)
{
    return valid_symbols[SECTION_PREP] ||
           valid_symbols[SECTION_GENERATE_BUILDREQUIRES] ||
           valid_symbols[SECTION_CONF] || valid_symbols[SECTION_BUILD] ||
           valid_symbols[SECTION_INSTALL] || valid_symbols[SECTION_CHECK] ||
           valid_symbols[SECTION_CLEAN];
}

/**
 * @brief Check if we're inside a scriptlet context
 *
 * Returns true if any scriptlet-specific conditional token is valid,
 * indicating we're inside a scriptlet section like %build, %install, etc.
 * In scriptlet context, parametric macros should NOT match because we
 * want shell semantics (macros expand inline, rest is shell arguments).
 */
static inline bool in_scriptlet_context(const bool *valid_symbols)
{
    return valid_symbols[SCRIPTLET_IF] || valid_symbols[SCRIPTLET_IFARCH] ||
           valid_symbols[SCRIPTLET_IFNARCH] || valid_symbols[SCRIPTLET_IFOS] ||
           valid_symbols[SCRIPTLET_IFNOS];
}

/**
 * @brief Scriptlet section keyword to token mapping
 */
struct SectionKeyword {
    const char *name;
    size_t len;
    enum TokenType token;
};

static const struct SectionKeyword SECTION_KEYWORDS_MAP[] = {
    {"prep", 4, SECTION_PREP},
    {"generate_buildrequires", 22, SECTION_GENERATE_BUILDREQUIRES},
    {"conf", 4, SECTION_CONF},
    {"build", 5, SECTION_BUILD},
    {"install", 7, SECTION_INSTALL},
    {"check", 5, SECTION_CHECK},
    {"clean", 5, SECTION_CLEAN},
    {NULL, 0, 0} /* End marker */
};

/**
 * @brief Look up a section keyword and return its token type
 *
 * @param id The identifier to look up
 * @param len Length of the identifier
 * @return Pointer to matching SectionKeyword, or NULL if not found
 */
static const struct SectionKeyword *lookup_section_keyword(const char *id,
                                                           size_t len)
{
    for (const struct SectionKeyword *kw = SECTION_KEYWORDS_MAP;
         kw->name != NULL;
         kw++) {
        if (strequal(kw->name, id, len)) {
            return kw;
        }
    }
    return NULL;
}

/* ========================================================================== */
/* MAIN SCAN LOGIC                                                            */
/* ========================================================================== */

/**
 * @brief Check if identifier is a conditional keyword
 */
static bool is_cond_keyword(const char *id, size_t len)
{
    for (size_t i = 0; i < NUM_COND_KEYWORDS; i++) {
        if (strequal(COND_KEYWORDS[i].name, id, len)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Try to match a conditional keyword token
 *
 * @param scanner The scanner state
 * @param lexer The lexer
 * @param valid_symbols Valid token types
 * @param keyword The keyword that was peeked
 * @param keyword_len Length of the keyword
 * @return true if a conditional token was matched
 */
static bool try_scan_conditional(struct Scanner *scanner,
                                 TSLexer *lexer,
                                 const bool *valid_symbols,
                                 const char *keyword,
                                 size_t keyword_len)
{
    for (size_t i = 0; i < NUM_COND_KEYWORDS; i++) {
        const struct CondKeyword *kw = &COND_KEYWORDS[i];

        if (!strequal(kw->name, keyword, keyword_len)) {
            continue;
        }

        struct CondTokens ctx = {
            .top = kw->top,
            .subsection = kw->subsection,
            .scriptlet = kw->scriptlet,
            .files = kw->files,
            .top_valid = valid_symbols[kw->top],
            .subsection_valid = valid_symbols[kw->subsection],
            .scriptlet_valid = valid_symbols[kw->scriptlet],
            .files_valid = valid_symbols[kw->files],
        };

        if (!ctx.top_valid && !ctx.subsection_valid && !ctx.scriptlet_valid &&
            !ctx.files_valid) {
            return false;
        }

        lexer->mark_end(lexer);
        lexer->result_symbol =
            select_conditional_token_type(scanner, lexer, &ctx);
        return true;
    }

    return false;
}

/**
 * @brief Try to match a parametric macro name token
 *
 * Parametric macros consume arguments on the same line. They are only
 * matched when allow_parametric is true (determined by caller based on
 * context - not in scriptlet sections).
 *
 * @param lexer The lexer
 * @param allow_parametric Whether parametric macros are allowed in this context
 * @param keyword The keyword that was peeked
 * @param keyword_len Length of the keyword
 * @return true if a parametric macro token was matched
 */
static bool try_scan_parametric_macro(TSLexer *lexer,
                                      bool allow_parametric,
                                      const char *keyword,
                                      size_t keyword_len)
{
    if (!allow_parametric) {
        return false;
    }

    /* Exclude reserved keywords and file directive keywords */
    if (is_keyword(keyword, keyword_len) ||
        is_files_keyword(keyword, keyword_len) ||
        is_patch_legacy(keyword, keyword_len) || is_nil(keyword, keyword_len)) {
        return false;
    }

    /* Must be followed by horizontal whitespace (arguments) */
    if (!is_horizontal_space(lexer->lookahead)) {
        return false;
    }

    lexer->mark_end(lexer);
    lexer->result_symbol = PARAMETRIC_MACRO_NAME;
    return true;
}

/**
 * @brief Consume '%' and read the following identifier
 *
 * Reads a percent-prefixed identifier from the lexer, consuming both the '%'
 * and the identifier characters. This is used to peek at what keyword follows
 * '%' so we can route to the correct token handler (conditional vs parametric
 * macro vs simple macro).
 *
 * Example: For input `%configure --prefix=/usr`
 *   - Consumes: `%configure`
 *   - Returns: id_buf = "configure", id_len = 9
 *   - Lexer position: at the space before `--prefix`
 *
 * Note: If the identifier is longer than buf_size, it will still be fully
 * consumed from the lexer, but id_buf will be truncated. The id_len will
 * reflect the true length.
 *
 * @param lexer The lexer (will be advanced past '%' and identifier)
 * @param id_buf Buffer to store the identifier (without '%')
 * @param buf_size Size of id_buf
 * @param[out] id_len Actual length of the identifier
 * @return true if '%' followed by a valid identifier was found
 */
static bool consume_percent_and_identifier(TSLexer *lexer,
                                           char *id_buf,
                                           size_t buf_size,
                                           size_t *id_len)
{
    *id_len = 0;

    if (lexer->lookahead != '%') {
        return false;
    }

    lexer->advance(lexer, false); /* consume '%' */

    if (!is_identifier_start(lexer->lookahead)) {
        return false;
    }

    /* Read the identifier */
    while (is_identifier_char(lexer->lookahead) && *id_len < buf_size - 1) {
        id_buf[(*id_len)++] = (char)lexer->lookahead;
        lexer->advance(lexer, false);
    }

    /* Consume any remaining chars if buffer was too small */
    while (is_identifier_char(lexer->lookahead)) {
        lexer->advance(lexer, false);
        (*id_len)++;
    }

    id_buf[*id_len < buf_size ? *id_len : buf_size - 1] = '\0';
    return *id_len > 0;
}

/**
 * @brief Skip leading whitespace before a potential macro
 *
 * Advances the lexer past spaces and tabs. Does NOT skip newlines - those
 * are meaningful for line-based constructs and may be expected by grammar.
 *
 * @param lexer The lexer
 */
static void skip_leading_whitespace(TSLexer *lexer)
{
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        lexer->advance(lexer, true);
    }
}

/**
 * @brief Main scanning function for RPM spec tokens
 *
 * This is the primary entry point for external token recognition. It handles
 * tokens that cannot be expressed in the grammar DSL alone, such as tokens
 * requiring keyword exclusion, context-aware lookahead, or balanced delimiter
 * tracking.
 *
 * Token categories (in priority order):
 *
 * 1. **Percent-prefixed tokens** (conditionals, parametric macros)
 *    Scanner consumes the '%' as part of the token:
 *    - Conditionals: `%if`, `%ifarch`, `%else`, `%endif`, etc.
 *    - Parametric macros: `%configure --prefix=/usr` (only at line start)
 *
 *    These require peeking at the keyword after '%' to route correctly.
 *    Conditionals have priority over parametric macros.
 *    MUST be checked first so section keywords are recognized during
 *    error recovery.
 *
 * 2. **Simple macro tokens** (SIMPLE_MACRO, NEGATED_MACRO, etc.)
 *    Grammar handles '%', scanner matches the identifier:
 *    - `%name` -> grammar matches '%', scanner matches 'name'
 *    - `%{name}` -> handled entirely by grammar
 *
 * 3. **Contextual tokens** (EXPAND_CODE, SCRIPT_CODE)
 *    Only valid inside specific constructs:
 *    - EXPAND_CODE: inside `%{expand:...}`
 *    - SCRIPT_CODE: inside `%(...)`
 *
 *    These are checked LAST because they are greedy and would consume
 *    section keywords during error recovery if checked earlier.
 *
 * @param scanner The scanner state (for lookahead caching)
 * @param lexer The tree-sitter lexer
 * @param valid_symbols Array indicating which tokens are valid at this position
 * @return true if a token was matched, false otherwise
 */
static inline bool
rpmspec_scan(struct Scanner *scanner, TSLexer *lexer, const bool *valid_symbols)
{
    /*
     * 0. Handle newlines for line-sensitive contexts
     *
     * When the grammar expects a newline (valid_symbols[NEWLINE] is true),
     * we emit the NEWLINE token to give it priority over extras.
     *
     * When the grammar doesn't expect a newline, skip them as whitespace.
     * This ensures newlines are consumed as extras in contexts where they
     * don't matter (like between statements).
     *
     * IMPORTANT: Don't skip whitespace when content tokens (EXPAND_CODE,
     * SCRIPT_CODE) are valid - they need to capture whitespace as content.
     */
    if (!valid_symbols[EXPAND_CODE] && !valid_symbols[SCRIPT_CODE]) {
        while (isspace(lexer->lookahead)) {
            if (lexer->lookahead == '\n') {
                if (valid_symbols[NEWLINE]) {
                    /* Emit newline token */
                    lexer->advance(lexer, false);
                    lexer->mark_end(lexer);
                    lexer->result_symbol = NEWLINE;
                    return true;
                }
                /* Skip newline as whitespace */
            } else if (lexer->lookahead == '\r') {
                if (valid_symbols[NEWLINE]) {
                    /* Handle \r\n or just \r */
                    lexer->advance(lexer, false);
                    if (lexer->lookahead == '\n') {
                        lexer->advance(lexer, false);
                    }
                    lexer->mark_end(lexer);
                    lexer->result_symbol = NEWLINE;
                    return true;
                }
                /* Skip carriage return as whitespace */
            }
            lexer->advance(lexer, true); /* skip */
        }
    }

    /*
     * 1. Percent-prefixed tokens - scanner handles the '%'
     *
     * This handles:
     * - Conditionals (%if, %else, etc.)
     * - Parametric macros (%configure)
     * - Scriptlet sections (%prep, %build, %conf, etc.)
     *
     * Section tokens are checked here (not separately) to prevent %conf
     * from matching %configure. We consume %identifier once, then check
     * in order: conditionals, sections, parametric macros.
     */
    bool conditionals_valid = any_conditional_valid(valid_symbols);
    bool parametric_valid = valid_symbols[PARAMETRIC_MACRO_NAME];
    bool sections_valid = any_section_token_valid(valid_symbols);

    if (conditionals_valid || parametric_valid || sections_valid) {
        /*
         * Determine if parametric macros should match in this context.
         *
         * In scriptlet context (inside %build, %install, etc.), we use shell
         * semantics: macros expand inline and the rest is shell arguments.
         *   %gobuild -o foo bar   <- %gobuild is simple, "-o foo bar" is shell
         *
         * Outside scriptlet context (top-level, inside %ifarch, etc.), we use
         * macro semantics: the macro consumes arguments.
         *   %bcond_without luajit <- %bcond_without is parametric with arg
         */
        bool allow_parametric = !in_scriptlet_context(valid_symbols);

        /* Skip any remaining whitespace */
        skip_leading_whitespace(lexer);

        if (lexer->lookahead == '%') {
            lexer->mark_end(lexer);

            char keyword[64];
            size_t keyword_len = 0;

            if (consume_percent_and_identifier(lexer,
                                               keyword,
                                               sizeof(keyword),
                                               &keyword_len)) {
                /* Try conditional first (highest priority) */
                if (conditionals_valid &&
                    is_cond_keyword(keyword, keyword_len)) {
                    if (try_scan_conditional(scanner,
                                             lexer,
                                             valid_symbols,
                                             keyword,
                                             keyword_len)) {
                        return true;
                    }
                }

                /* Try section token (with word boundary check) */
                if (sections_valid && !is_identifier_char(lexer->lookahead)) {
                    const struct SectionKeyword *kw =
                        lookup_section_keyword(keyword, keyword_len);
                    if (kw != NULL && valid_symbols[kw->token]) {
                        lexer->mark_end(lexer);
                        lexer->result_symbol = kw->token;
                        return true;
                    }
                }

                /* Try parametric macro */
                if (parametric_valid) {
                    if (try_scan_parametric_macro(lexer,
                                                  allow_parametric,
                                                  keyword,
                                                  keyword_len)) {
                        return true;
                    }
                }
            }
        }
    }

    /*
     * 2. Simple macro tokens - grammar handles '%', scanner matches identifier
     */
    bool macros_valid =
        valid_symbols[SIMPLE_MACRO] || valid_symbols[NEGATED_MACRO] ||
        valid_symbols[SPECIAL_MACRO] || valid_symbols[ESCAPED_PERCENT];

    if (macros_valid) {
        return scan_macro(lexer, valid_symbols);
    }

    /*
     * 3. Contextual content tokens - only valid inside specific constructs
     *
     * These are checked LAST because they are greedy and would consume
     * section keywords during error recovery if checked earlier.
     * - EXPAND_CODE: content inside %{expand:...}
     * - SCRIPT_CODE: content inside %(...)
     */
    if (valid_symbols[EXPAND_CODE] && scan_expand_content(lexer)) {
        lexer->result_symbol = EXPAND_CODE;
        return true;
    }

    if (valid_symbols[SCRIPT_CODE] && scan_shell_content(lexer)) {
        lexer->result_symbol = SCRIPT_CODE;
        return true;
    }

    return false;
}

/* ========================================================================== */
/* TREE-SITTER API                                                            */
/* ========================================================================== */

/**
 * @brief Serializes the scanner state into a byte buffer
 *
 * This function copies the complete state of the scanner into the given byte
 * buffer and returns the number of bytes written. Used by Tree-sitter for
 * incremental parsing and error recovery.
 *
 * @param scanner The scanner instance to serialize
 * @param buffer The byte buffer to write the state to
 * @return The number of bytes written to the buffer
 */
static inline unsigned rpmspec_serialize(struct Scanner *scanner, char *buffer)
{
    /* Serialize the lookahead cache (2 bytes) */
    if (2 > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
        return 0;
    }

    buffer[0] = scanner->lookahead_cache_valid ? 1 : 0;
    buffer[1] = scanner->lookahead_has_section ? 1 : 0;

    return 2;
}

/**
 * @brief Deserializes the scanner state from a byte buffer
 *
 * This function restores the state of the scanner based on the bytes that were
 * previously written by the serialize function. Used by Tree-sitter for
 * incremental parsing and error recovery.
 *
 * @param scanner The scanner instance to restore state to
 * @param buffer The byte buffer containing the serialized state
 * @param length The number of bytes to read from the buffer
 */
static inline void rpmspec_deserialize(struct Scanner *scanner,
                                       const char *buffer,
                                       unsigned length)
{
    /* Clear cache by default */
    scanner->lookahead_cache_valid = false;
    scanner->lookahead_has_section = false;

    if (length < 2) {
        return;
    }

    /* Deserialize the lookahead cache */
    scanner->lookahead_cache_valid = buffer[0] != 0;
    scanner->lookahead_has_section = buffer[1] != 0;
}

/**
 * @brief Creates and initializes a new scanner instance
 *
 * This function is called by Tree-sitter to create a new external scanner
 * instance. It allocates memory and initializes the scanner state.
 *
 * @return A pointer to the newly created scanner instance
 */
void *tree_sitter_rpmspec_external_scanner_create(void)
{
    struct Scanner *scanner = ts_calloc(1, sizeof(struct Scanner));

    return scanner;
}

/**
 * @brief Destroys a scanner instance and frees its memory
 *
 * This function is called by Tree-sitter to clean up and destroy an external
 * scanner instance, releasing all allocated memory.
 *
 * @param payload The scanner instance to destroy (cast from void*)
 */
void tree_sitter_rpmspec_external_scanner_destroy(void *payload)
{
    struct Scanner *scanner = (struct Scanner *)payload;

    ts_free(scanner);
}

/**
 * @brief Tree-sitter API function for serializing scanner state
 *
 * This is the Tree-sitter external scanner API function that delegates
 * to the internal serialization implementation.
 *
 * @param payload The scanner instance (cast from void*)
 * @param buffer The buffer to write serialized state to
 * @return The number of bytes written
 */
unsigned tree_sitter_rpmspec_external_scanner_serialize(void *payload,
                                                        char *buffer)
{
    struct Scanner *scanner = (struct Scanner *)payload;

    return rpmspec_serialize(scanner, buffer);
}

/**
 * @brief Tree-sitter API function for deserializing scanner state
 *
 * This is the Tree-sitter external scanner API function that delegates
 * to the internal deserialization implementation.
 *
 * @param payload The scanner instance (cast from void*)
 * @param buffer The buffer containing serialized state
 * @param length The number of bytes to read from the buffer
 */
void tree_sitter_rpmspec_external_scanner_deserialize(void *payload,
                                                      const char *buffer,
                                                      unsigned length)
{
    struct Scanner *scanner = (struct Scanner *)payload;

    rpmspec_deserialize(scanner, buffer, length);
}

/**
 * @brief Tree-sitter API function for scanning tokens
 *
 * This is the main Tree-sitter external scanner API function that delegates
 * to the internal scanning implementation. Called by Tree-sitter during parsing
 * to recognize external tokens.
 *
 * @param payload The scanner instance (cast from void*)
 * @param lexer The Tree-sitter lexer instance
 * @param valid_symbols Array indicating which token types are valid at this
 * position
 * @return true if a token was successfully recognized, false otherwise
 */
bool tree_sitter_rpmspec_external_scanner_scan(void *payload,
                                               TSLexer *lexer,
                                               const bool *valid_symbols)
{
    struct Scanner *scanner = (struct Scanner *)payload;

    return rpmspec_scan(scanner, lexer, valid_symbols);
}
