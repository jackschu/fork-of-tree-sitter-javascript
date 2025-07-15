#include "tree_sitter/parser.h"

#include <stdio.h>
#include <wctype.h>

enum TokenType {
    AUTOMATIC_SEMICOLON,
    TEMPLATE_CHARS,
    TERNARY_QMARK,
    SHORTHAND_ARROW,
    HTML_COMMENT,
    LOGICAL_OR,
    LEFT_PARENTHESIS,
    LEFT_SQUARE_BRACKET,
    LEFT_CURLY_BRACE,
    ESCAPE_SEQUENCE,
    REGEX_PATTERN,
    JSX_TEXT,
    LET_IDENTIFIER,
};

void *tree_sitter_javascript_external_scanner_create() { return NULL; }

void tree_sitter_javascript_external_scanner_destroy(void *p) {}

unsigned tree_sitter_javascript_external_scanner_serialize(void *payload, char *buffer) { return 0; }

void tree_sitter_javascript_external_scanner_deserialize(void *p, const char *b, unsigned n) {}

static inline void advance(TSLexer *lexer) {
    lexer->advance(lexer, false);
}

static inline void skip(TSLexer *lexer) {
    lexer->advance(lexer, true);
}

static bool scan_template_chars(TSLexer *lexer) {
    lexer->result_symbol = TEMPLATE_CHARS;
    for (bool has_content = false;; has_content = true) {
        lexer->mark_end(lexer);
        switch (lexer->lookahead) {
            case '`':
                return has_content;
            case '\0':
                return false;
            case '$':
                advance(lexer);
                if (lexer->lookahead == '{') {
                    return has_content;
                }
                break;
            case '\\':
                return has_content;
            default:
                advance(lexer);
        }
    }
}

typedef enum {
    REJECT,     // We encountered non-wspace, non-comment after a '/' this is a regex or syntax error, concluded rejection
    ACCEPT,     // We encountered non-wspace, non-comment after a '/' this is a regex or syntax error, concluded acceptance
    NO_NEWLINE, // Unclear if ASI will be legal, consumed a single-line block comment, continue
    NEWLINE,    // Scanned a newline, ASI is likely legal if the upcoming characters are judged to be illegal
} WhitespaceResult;

static WhitespaceResult scan_whitespace_and_comments(TSLexer *lexer, bool *scanned_comment, const bool *valid_symbols, bool repeatedly) {
    bool saw_newline = false;

    for (;;) {
        while (iswspace(lexer->lookahead)) {
            if (lexer->lookahead == '\n' || lexer->lookahead == 0x2028 || lexer->lookahead == 0x2029) {
                saw_newline = true;
            }
            advance(lexer);
        }

        if (lexer->lookahead == '/') {
            advance(lexer);

            if (lexer->lookahead == '/') {
                advance(lexer);
                while (!lexer->eof(lexer) && lexer->lookahead != '\n' && lexer->lookahead != 0x2028 &&
                       lexer->lookahead != 0x2029) {
                    advance(lexer);
                }
                saw_newline = true;
                *scanned_comment = true;
            } else if (lexer->lookahead == '*') {
                advance(lexer);
                while (!lexer->eof(lexer)) {
                    if (lexer->lookahead == '*') {
                        advance(lexer);
                        if (lexer->lookahead == '/') {
                            advance(lexer);
                            *scanned_comment = true;
                            break;
                        }
                    } else if (lexer->lookahead == '\n' || lexer->lookahead == 0x2028 || lexer->lookahead == 0x2029) {
                        saw_newline = true;
                        advance(lexer);
                    } else {
                        advance(lexer);
                    }
                }
            } else {
                // We've detected regex or division while scanning a comment
                // If LOGICAL_OR is not allowed we assume we must be looking at regex and accept if we've passed a newline
                if (repeatedly) {
                    return REJECT;
                }
                if (!valid_symbols[LOGICAL_OR] && saw_newline) {
                    return ACCEPT;
                }
                return REJECT;
            }
        } else {
            if (!repeatedly) {
                return saw_newline ? NEWLINE : NO_NEWLINE;
            }
            if (!iswspace(lexer->lookahead) || lexer->lookahead != '/') {
                return ACCEPT;
            }
        }
    }
}

static bool scan_automatic_semicolon(TSLexer *lexer, bool *scanned_comment, const bool *valid_symbols) {
    lexer->result_symbol = AUTOMATIC_SEMICOLON;
    lexer->mark_end(lexer);

    for (;;) {
        if (lexer->eof(lexer)) {
            return true;
        }

        if (lexer->lookahead == '/') {
            WhitespaceResult result = scan_whitespace_and_comments(lexer, scanned_comment, valid_symbols, false);
            if (result == NEWLINE) {
                break;
            }
            if (result == ACCEPT) {
                return true;
            } 
            if (result == REJECT) {
                return false;
            }
        }

        if (lexer->lookahead == '}') {
            return true;
        }

        if (lexer->is_at_included_range_start(lexer)) {
            return true;
        }

        if (lexer->lookahead == '\n' || lexer->lookahead == 0x2028 || lexer->lookahead == 0x2029) {
            break;
        }

        if (!iswspace(lexer->lookahead)) {
            return false;
        }

        skip(lexer);
    }

    if (scan_whitespace_and_comments(lexer, scanned_comment, valid_symbols, false) == REJECT) {
        return false;
    }

    switch (lexer->lookahead) {
        case '`':
        case ',':
        case ':':
        case ';':
        case '*':
        case '%':
        case '>':
        case '<':
        case '=':
        case '?':
        case '^':
        case '|':
        case '&':
        case '/':
            return false;

        case '[':
            return !valid_symbols[LEFT_SQUARE_BRACKET];
        case '(':
            return !valid_symbols[LEFT_PARENTHESIS];
        case '{':
            return !valid_symbols[LEFT_CURLY_BRACE];

        // Insert a semicolon before decimals literals but not otherwise.
        case '.':
            skip(lexer);
            return iswdigit(lexer->lookahead);

        // Insert a semicolon before `--` and `++`, but not before binary `+` or `-`.
        case '+':
            skip(lexer);
            return lexer->lookahead == '+';
        case '-':
            skip(lexer);
            return lexer->lookahead == '-';

        // Don't insert a semicolon before `!=`, but do insert one before a unary `!`.
        case '!':
            skip(lexer);
            return lexer->lookahead != '=';

        // Don't insert a semicolon before `in` or `instanceof`, but do insert one
        // before an identifier.
        case 'i':
            skip(lexer);

            if (lexer->lookahead != 'n') {
                return true;
            }
            skip(lexer);

            if (!iswalpha(lexer->lookahead)) {
                return false;
            }

            for (unsigned i = 0; i < 8; i++) {
                if (lexer->lookahead != "stanceof"[i]) {
                    return true;
                }
                skip(lexer);
            }

            if (!iswalpha(lexer->lookahead)) {
                return false;
            }
            break;

        default:
            break;
    }

    return true;
}

static bool scan_ternary_qmark(TSLexer *lexer) {
    if (lexer->lookahead == '?') {
        advance(lexer);

        if (lexer->lookahead == '?') {
            return false;
        }

        lexer->mark_end(lexer);
        lexer->result_symbol = TERNARY_QMARK;

        if (lexer->lookahead == '.') {
            advance(lexer);
            if (iswdigit(lexer->lookahead)) {
                return true;
            }
            return false;
        }
        return true;
    }
    return false;
}

static bool scan_html_comment(TSLexer *lexer) {
    while (iswspace(lexer->lookahead) || lexer->lookahead == 0x2028 || lexer->lookahead == 0x2029) {
        skip(lexer);
    }

    const char *comment_start = "<!--";
    const char *comment_end = "-->";

    if (lexer->lookahead == '<') {
        for (unsigned i = 0; i < 4; i++) {
            if (lexer->lookahead != comment_start[i]) {
                return false;
            }
            advance(lexer);
        }
    } else if (lexer->lookahead == '-') {
        for (unsigned i = 0; i < 3; i++) {
            if (lexer->lookahead != comment_end[i]) {
                return false;
            }
            advance(lexer);
        }
    } else {
        return false;
    }

    while (lexer->lookahead != 0 && lexer->lookahead != '\n' && lexer->lookahead != 0x2028 &&
           lexer->lookahead != 0x2029) {
        advance(lexer);
    }

    lexer->result_symbol = HTML_COMMENT;
    lexer->mark_end(lexer);

    return true;
}

static bool scan_jsx_text(TSLexer *lexer) {
    // saw_text will be true if we see any non-whitespace content, or any whitespace content that is not a newline and
    // does not immediately follow a newline.
    bool saw_text = false;
    // at_newline will be true if we are currently at a newline, or if we are at whitespace that is not a newline but
    // immediately follows a newline.
    bool at_newline = false;

    while (lexer->lookahead != 0 && lexer->lookahead != '<' && lexer->lookahead != '>' && lexer->lookahead != '{' &&
           lexer->lookahead != '}' && lexer->lookahead != '&') {
        bool is_wspace = iswspace(lexer->lookahead);
        if (lexer->lookahead == '\n') {
            at_newline = true;
        } else {
            // If at_newline is already true, and we see some whitespace, then it must stay true.
            // Otherwise, it should be false.
            //
            // See the table below to determine the logic for computing `saw_text`.
            //
            // |------------------------------------|
            // | at_newline | is_wspace | saw_text  |
            // |------------|-----------|-----------|
            // | false (0)  | false (0) | true  (1) |
            // | false (0)  | true  (1) | true  (1) |
            // | true  (1)  | false (0) | true  (1) |
            // | true  (1)  | true  (1) | false (0) |
            // |------------------------------------|

            at_newline &= is_wspace;
            if (!at_newline) {
                saw_text = true;
            }
        }

        advance(lexer);
    }

    lexer->result_symbol = JSX_TEXT;
    return saw_text;
}

static bool scan_shorthand_arrow(TSLexer *lexer) {
    lexer->result_symbol = SHORTHAND_ARROW;

    for (;;) {
        if (!iswspace(lexer->lookahead)) {
            break;
        }
        skip(lexer);
    }
    if (lexer->lookahead == '=') {
        advance(lexer);
        if (lexer->lookahead == '>') {
            advance(lexer);
            lexer->mark_end(lexer);
            for (;;) {
                if (!iswspace(lexer->lookahead)) {
                    break;
                }
                skip(lexer);
            }
            return lexer->lookahead != '{';            
        }
    }
    return false;
}


// poor man's assumption: these are the characters that are not ID_Start or ID_Continue
static bool is_non_id_start_continue(wint_t wc) {
    switch (wc) {
        case L'#':
        case L'%':
        case L'&':
        case L'\'':
        case L'(':
        case L')':
        case L'*':
        case L'+':
        case L',':
        case L'-':
        case L'.':
        case L'/':
        case L':':
        case L';':
        case L'<':
        case L'=':
        case L'>':
        case L'?':
        case L'@':
        case L'[':
        case L'\\':
        case L']':
        case L'^':
        case L'`':
        case L'{':
        case L'|':
        case L'}':
        case L'~':
            return true;
        default:
            return !iswalnum(wc);
    }
}

// from musl
static size_t wcslen(const wchar_t *s)
{
	const wchar_t *a;
	for (a=s; *s; s++);
	return s-a;
}
static int wcsncmp(const wchar_t *l, const wchar_t *r, size_t n)
{
	for (; n && *l==*r && *l && *r; n--, l++, r++);
	return n ? (*l < *r ? -1 : *l > *r) : 0;
}
static bool is_widestring_match(const wchar_t *str, const wchar_t *other) {
    // Get the lengths of both widestrings
    size_t str_len = wcslen(str);
    size_t other_len = wcslen(other);

    if (other_len != str_len) {
        return false;
    }


    if (wcsncmp(str, other, str_len) == 0) {
        return true;
    } else {
        return false;
    }
}

const wchar_t *const RESERVED_WORDS[] = {
    L"await",
    L"break",
    L"case",
    L"catch",
    L"class",
    L"const",
    L"continue",
    L"debugger",
    L"default",
    L"delete",
    L"do",
    L"else",
    L"enum",
    L"export",
    L"extends",
    L"false",
    L"finally",
    L"for",
    L"function",
    L"if",
    L"import",
    L"in",
    L"instanceof",
    L"new",
    L"null",
    L"return",
    L"super",
    L"switch",
    L"this",
    L"throw",
    L"true",
    L"try",
    L"typeof",
    L"var",
    L"void",
    L"while",
    L"with",
    L"yield",
};


static bool scan_let(TSLexer *lexer, const bool *valid_symbols) {
    if (lexer->lookahead != 'l') {
        return false;
    }
    advance(lexer);
    if (lexer->lookahead != 'e') {
        return false;
    }
    advance(lexer);
    if (lexer->lookahead != 't') {
        return false;
    }
    advance(lexer);

    lexer->mark_end(lexer);

    bool _unused;
    if (scan_whitespace_and_comments(lexer, &_unused, valid_symbols, true) == REJECT) {
        lexer->result_symbol = LET_IDENTIFIER;
        return true;
    }

    while (!lexer->eof(lexer) && iswspace(lexer->lookahead)) {
        advance(lexer);
    }

    if (lexer->lookahead == '[' || lexer->lookahead == '{') {
        return false;
    }
    wchar_t current_word_buffer[11]; // Max 10 chars + null terminator
    size_t current_word_len = 0;

    // poor mans assumption: digits are the sole characters in ID_Continue but not ID_Start
    if (iswdigit(lexer->lookahead)) {
        lexer->result_symbol = LET_IDENTIFIER;
        return true;
    }
    wint_t c;
    while (current_word_len < 10) {
        c = lexer->lookahead;
        if(lexer->eof(lexer)) {
            break;
        }
        
        if (is_non_id_start_continue(c) && c != L'_' && c != L'$') {
            break;
        }

        advance(lexer);

        if (iswspace(c)) {
            break;
        }
        current_word_buffer[current_word_len++] = c;
    }

    if (current_word_len == 0) {
        lexer->result_symbol = LET_IDENTIFIER;
        return true;
    }
    current_word_buffer[current_word_len] = L'\0';
                
    int n = sizeof(RESERVED_WORDS) / sizeof(RESERVED_WORDS[0]);
    for (int i = 0; i < n; i++)  {
        if (is_widestring_match(current_word_buffer, RESERVED_WORDS[i])) {
            lexer->result_symbol = LET_IDENTIFIER;
            return true;
        }
    }

    return false;
}


bool tree_sitter_javascript_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    if (valid_symbols[TEMPLATE_CHARS]) {
        if (valid_symbols[AUTOMATIC_SEMICOLON]) {
            return false;
        }
        return scan_template_chars(lexer);
    }

    if (valid_symbols[JSX_TEXT] && scan_jsx_text(lexer)) {
        return true;
    }

    if (valid_symbols[AUTOMATIC_SEMICOLON]) {
        bool scanned_comment = false;
        bool ret = scan_automatic_semicolon(lexer, &scanned_comment, valid_symbols);
        if (!ret && !scanned_comment && valid_symbols[TERNARY_QMARK] && lexer->lookahead == '?') {
            return scan_ternary_qmark(lexer);
        }
        if (!ret && valid_symbols[SHORTHAND_ARROW] && lexer->lookahead == '=') {
            return scan_shorthand_arrow(lexer);
        }
        if (!ret && valid_symbols[LET_IDENTIFIER] && lexer->lookahead == 'l') {
            return scan_let(lexer, valid_symbols);
        }

        return ret;
    }

    if (valid_symbols[LET_IDENTIFIER]) {
        while(!lexer->eof(lexer) && iswspace(lexer->lookahead)) {
            skip(lexer);
        }
        if (lexer->lookahead == 'l') {
            return scan_let(lexer, valid_symbols);
        }
    }

    if (valid_symbols[TERNARY_QMARK]) {
        for (;;) {
            if (!iswspace(lexer->lookahead)) {
                break;
            }
            skip(lexer);
        }
        if (lexer->lookahead == '?') {
            return scan_ternary_qmark(lexer);
        }
    }

    if (valid_symbols[SHORTHAND_ARROW]) {
        return scan_shorthand_arrow(lexer);
    }

    if (valid_symbols[HTML_COMMENT] && !valid_symbols[LOGICAL_OR] && !valid_symbols[ESCAPE_SEQUENCE] &&
        !valid_symbols[REGEX_PATTERN]) {
        return scan_html_comment(lexer);
    }

    return false;
}
