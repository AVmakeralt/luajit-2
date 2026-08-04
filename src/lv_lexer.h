/* lv_lexer.h — Lua 5.4 tokenizer.
 *
 * Tokenizes a UTF-8 source string into a stream of Lua tokens.
 * Supports all Lua 5.4 lexical syntax:
 *   - Identifiers (incl. reserved keywords)
 *   - Numeric literals (integers, hex, floats, hex floats, exponents)
 *   - String literals (single/double quoted, long [[ ]] brackets)
 *   - Long comments --[[ ]] and line comments --
 *   - All operators and punctuation
 *   - Varargs ...
 */

#ifndef LV_LEXER_H
#define LV_LEXER_H

#include "lv.h"

/* ---- Token types ---- */
typedef enum {
    LV_TOK_EOF = 0,

    /* Literals */
    LV_TOK_NIL,
    LV_TOK_TRUE,
    LV_TOK_FALSE,
    LV_TOK_NUMBER,      /* int or float (see token.is_float) */
    LV_TOK_STRING,      /* value in token.str / str_len */
    LV_TOK_NAME,        /* identifier; value in token.name */
    LV_TOK_DOTS,        /* ... */

    /* Operators */
    LV_TOK_PLUS,        /* + */
    LV_TOK_MINUS,       /* - */
    LV_TOK_STAR,        /* * */
    LV_TOK_SLASH,       /* / */
    LV_TOK_DSLASH,      /* // */
    LV_TOK_PERCENT,     /* % */
    LV_TOK_CARET,       /* ^ */
    LV_TOK_HASH,        /* # */
    LV_TOK_AMP,         /* & */
    LV_TOK_PIPE,        /* | */
    LV_TOK_TILDE,       /* ~ */
    LV_TOK_DLT,         /* << */
    LV_TOK_DGT,         /* >> */
    LV_TOK_EQEQ,        /* == */
    LV_TOK_NOTEQ,       /* ~= */
    LV_TOK_LE,          /* <= */
    LV_TOK_GE,          /* >= */
    LV_TOK_LT,          /* < */
    LV_TOK_GT,          /* > */
    LV_TOK_ASSIGN,      /* = */
    LV_TOK_LPAREN,      /* ( */
    LV_TOK_RPAREN,      /* ) */
    LV_TOK_LBRACE,      /* { */
    LV_TOK_RBRACE,      /* } */
    LV_TOK_LBRACKET,    /* [ */
    LV_TOK_RBRACKET,    /* ] */
    LV_TOK_SEMI,        /* ; */
    LV_TOK_COLON,       /* : */
    LV_TOK_DCOLON,      /* :: */
    LV_TOK_COMMA,       /* , */
    LV_TOK_DOT,         /* . */
    LV_TOK_CONCAT,      /* .. */

    /* Keywords */
    LV_TOK_AND,
    LV_TOK_BREAK,
    LV_TOK_DO,
    LV_TOK_ELSE,
    LV_TOK_ELSEIF,
    LV_TOK_END,
    LV_TOK_FOR,
    LV_TOK_FUNCTION,
    LV_TOK_GOTO,
    LV_TOK_IF,
    LV_TOK_IN,
    LV_TOK_LOCAL,
    LV_TOK_NOT,
    LV_TOK_OR,
    LV_TOK_REPEAT,
    LV_TOK_RETURN,
    LV_TOK_THEN,
    LV_TOK_UNTIL,
    LV_TOK_WHILE,
} lv_token_kind_t;

/* ---- Token ---- */
typedef struct {
    lv_token_kind_t kind;
    int             line;
    int             column;

    /* For LV_TOK_NUMBER: the parsed value. is_float distinguishes
     * integer literals (e.g. 42) from float literals (e.g. 42.0, 1e10). */
    int64_t  ival;
    double   fval;
    bool     is_float;

    /* For LV_TOK_STRING: the decoded string contents (without quotes).
     * Owned by the token; freed by lv_token_fini(). */
    char  *str;
    size_t str_len;

    /* For LV_TOK_NAME: the identifier (NUL-terminated, owned). */
    char *name;
} lv_token_t;

void lv_token_fini(lv_token_t *tok);

/* ---- Lexer state ---- */
typedef struct {
    const char *src;        /* source text (does not take ownership) */
    size_t      src_len;
    size_t      pos;        /* current byte offset */
    int         line;
    int         column;

    /* Last token read by lv_lexer_next(). */
    lv_token_t cur;
    lv_token_t prev;        /* previous token, kept for error messages */
} lv_lexer_t;

/* Initialize a lexer over the given source text. Does not copy src. */
void lv_lexer_init(lv_lexer_t *lx, const char *src, size_t src_len);

/* Free lexer-owned resources (cur/prev token strings). */
void lv_lexer_fini(lv_lexer_t *lx);

/* Read the next token into lx->cur. The previous lx->cur is moved to
 * lx->prev (and its owned strings freed). */
void lv_lexer_next(lv_lexer_t *lx);

/* Peek at the current token without consuming. */
const lv_token_t *lv_lexer_peek(const lv_lexer_t *lx);

/* Human-readable name of a token kind (for error messages). */
const char *lv_token_kind_name(lv_token_kind_t k);

#endif /* LV_LEXER_H */
