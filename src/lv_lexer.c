/* lv_lexer.c — Lua 5.4 tokenizer implementation. */

#include "lv_lexer.h"
#include <ctype.h>
#include <stdarg.h>

/* ---- Keyword table ---- */
typedef struct { const char *kw; lv_token_kind_t kind; } kw_entry_t;
static const kw_entry_t k_keywords[] = {
    {"and",      LV_TOK_AND},
    {"break",    LV_TOK_BREAK},
    {"do",       LV_TOK_DO},
    {"else",     LV_TOK_ELSE},
    {"elseif",   LV_TOK_ELSEIF},
    {"end",      LV_TOK_END},
    {"for",      LV_TOK_FOR},
    {"function", LV_TOK_FUNCTION},
    {"goto",     LV_TOK_GOTO},
    {"if",       LV_TOK_IF},
    {"in",       LV_TOK_IN},
    {"local",    LV_TOK_LOCAL},
    {"nil",      LV_TOK_NIL},
    {"not",      LV_TOK_NOT},
    {"or",       LV_TOK_OR},
    {"repeat",   LV_TOK_REPEAT},
    {"return",   LV_TOK_RETURN},
    {"then",     LV_TOK_THEN},
    {"true",     LV_TOK_TRUE},
    {"false",    LV_TOK_FALSE},
    {"until",    LV_TOK_UNTIL},
    {"while",    LV_TOK_WHILE},
    {NULL, 0},
};

void lv_token_fini(lv_token_t *tok) {
    if (!tok) return;
    if (tok->str)  { lv_free(tok->str);  tok->str = NULL; }
    if (tok->name) { lv_free(tok->name); tok->name = NULL; }
}

const char *lv_token_kind_name(lv_token_kind_t k) {
    switch (k) {
    case LV_TOK_EOF:        return "<eof>";
    case LV_TOK_NIL:        return "nil";
    case LV_TOK_TRUE:       return "true";
    case LV_TOK_FALSE:      return "false";
    case LV_TOK_NUMBER:     return "<number>";
    case LV_TOK_STRING:     return "<string>";
    case LV_TOK_NAME:       return "<name>";
    case LV_TOK_DOTS:       return "...";
    case LV_TOK_PLUS:       return "+";
    case LV_TOK_MINUS:      return "-";
    case LV_TOK_STAR:       return "*";
    case LV_TOK_SLASH:      return "/";
    case LV_TOK_DSLASH:     return "//";
    case LV_TOK_PERCENT:    return "%";
    case LV_TOK_CARET:      return "^";
    case LV_TOK_HASH:       return "#";
    case LV_TOK_AMP:        return "&";
    case LV_TOK_PIPE:       return "|";
    case LV_TOK_TILDE:      return "~";
    case LV_TOK_DLT:        return "<<";
    case LV_TOK_DGT:        return ">>";
    case LV_TOK_EQEQ:       return "==";
    case LV_TOK_NOTEQ:      return "~=";
    case LV_TOK_LE:         return "<=";
    case LV_TOK_GE:         return ">=";
    case LV_TOK_LT:         return "<";
    case LV_TOK_GT:         return ">";
    case LV_TOK_ASSIGN:     return "=";
    case LV_TOK_LPAREN:     return "(";
    case LV_TOK_RPAREN:     return ")";
    case LV_TOK_LBRACE:     return "{";
    case LV_TOK_RBRACE:     return "}";
    case LV_TOK_LBRACKET:   return "[";
    case LV_TOK_RBRACKET:   return "]";
    case LV_TOK_SEMI:       return ";";
    case LV_TOK_COLON:      return ":";
    case LV_TOK_DCOLON:     return "::";
    case LV_TOK_COMMA:      return ",";
    case LV_TOK_DOT:        return ".";
    case LV_TOK_CONCAT:     return "..";
    case LV_TOK_AND:        return "and";
    case LV_TOK_BREAK:      return "break";
    case LV_TOK_DO:         return "do";
    case LV_TOK_ELSE:       return "else";
    case LV_TOK_ELSEIF:     return "elseif";
    case LV_TOK_END:        return "end";
    case LV_TOK_FOR:        return "for";
    case LV_TOK_FUNCTION:   return "function";
    case LV_TOK_GOTO:       return "goto";
    case LV_TOK_IF:         return "if";
    case LV_TOK_IN:         return "in";
    case LV_TOK_LOCAL:      return "local";
    case LV_TOK_NOT:        return "not";
    case LV_TOK_OR:         return "or";
    case LV_TOK_REPEAT:     return "repeat";
    case LV_TOK_RETURN:     return "return";
    case LV_TOK_THEN:       return "then";
    case LV_TOK_UNTIL:      return "until";
    case LV_TOK_WHILE:      return "while";
    }
    return "<unknown>";
}

/* ---- Lexer init/fini ---- */
void lv_lexer_init(lv_lexer_t *lx, const char *src, size_t src_len) {
    memset(lx, 0, sizeof(*lx));
    lx->src     = src;
    lx->src_len = src_len;
    lx->pos     = 0;
    lx->line    = 1;
    lx->column  = 1;
}

void lv_lexer_fini(lv_lexer_t *lx) {
    lv_token_fini(&lx->cur);
    lv_token_fini(&lx->prev);
}

/* ---- Helpers ---- */

static char peek_ch(lv_lexer_t *lx) {
    if (lx->pos >= lx->src_len) return '\0';
    return lx->src[lx->pos];
}

static char peek_ch2(lv_lexer_t *lx) {
    if (lx->pos + 1 >= lx->src_len) return '\0';
    return lx->src[lx->pos + 1];
}

static char next_ch(lv_lexer_t *lx) {
    if (lx->pos >= lx->src_len) return '\0';
    char c = lx->src[lx->pos++];
    if (c == '\n') {
        lx->line++;
        lx->column = 1;
    } else {
        lx->column++;
    }
    return c;
}

static void skip_ws_and_comments(lv_lexer_t *lx) {
    for (;;) {
        char c = peek_ch(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            next_ch(lx);
            continue;
        }
        if (c == '-' && peek_ch2(lx) == '-') {
            next_ch(lx); /* - */
            next_ch(lx); /* - */
            /* Check for long comment --[[ ... ]] (or [=[ ... ]=]) */
            if (peek_ch(lx) == '[') {
                /* Try to match [=* */
                size_t save = lx->pos;
                int level = 0;
                next_ch(lx); /* [ */
                while (peek_ch(lx) == '=') { next_ch(lx); level++; }
                if (peek_ch(lx) == '[') {
                    next_ch(lx);
                    /* Read until matching ]=* ] */
                    char close[64];
                    int cl = 0;
                    close[cl++] = ']';
                    for (int i = 0; i < level; i++) close[cl++] = '=';
                    close[cl++] = ']';
                    close[cl] = 0;
                    /* Scan for close */
                    for (;;) {
                        if (peek_ch(lx) == '\0') return;
                        if (peek_ch(lx) == ']') {
                            size_t p = lx->pos;
                            int match = 1;
                            for (int i = 0; i < cl; i++) {
                                if (lx->src[p + i] != close[i]) { match = 0; break; }
                            }
                            if (match) {
                                for (int i = 0; i < cl; i++) next_ch(lx);
                                break;
                            }
                        }
                        next_ch(lx);
                    }
                    continue;
                }
                /* Not a long comment — rewind and treat as line comment */
                lx->pos = save;
                lx->column--; /* approx; line comments don't need precise columns */
            }
            /* Line comment — read until newline */
            while (peek_ch(lx) != '\0' && peek_ch(lx) != '\n') next_ch(lx);
            continue;
        }
        return;
    }
}

static void set_token(lv_lexer_t *lx, lv_token_kind_t k) {
    lv_token_fini(&lx->prev);
    lx->prev = lx->cur;
    memset(&lx->cur, 0, sizeof(lx->cur));
    lx->cur.kind   = k;
    lx->cur.line   = lx->line;
    lx->cur.column = lx->column;
}

static int is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_ident_part(char c)  { return isalnum((unsigned char)c) || c == '_'; }

static void read_identifier(lv_lexer_t *lx) {
    size_t start = lx->pos;
    while (is_ident_part(peek_ch(lx))) next_ch(lx);
    size_t len = lx->pos - start;

    set_token(lx, LV_TOK_NAME);
    /* Check for keyword */
    for (const kw_entry_t *e = k_keywords; e->kw; e++) {
        size_t klen = strlen(e->kw);
        if (klen == len && strncmp(lx->src + start, e->kw, len) == 0) {
            lx->cur.kind = e->kind;
            return;
        }
    }
    /* Not a keyword — store as identifier */
    lx->cur.name = lv_strndup(lx->src + start, len);
}

/* Read a long string [[...]] or [=[...]=]. Returns 1 on success, 0 if
 * the leading [ wasn't actually a long-bracket (caller should treat as
 * regular token). The decoded contents are placed in tok->str. */
static int read_long_string(lv_lexer_t *lx, lv_token_t *tok) {
    /* Assumes lx->pos is just after the first '[' */
    int level = 0;
    while (peek_ch(lx) == '=') { next_ch(lx); level++; }
    if (peek_ch(lx) != '[') return 0;
    next_ch(lx); /* consume second [ */

    /* Skip first newline immediately after opening */
    if (peek_ch(lx) == '\n') next_ch(lx);

    /* Build the close bracket string */
    char close[64];
    int cl = 0;
    close[cl++] = ']';
    for (int i = 0; i < level; i++) close[cl++] = '=';
    close[cl++] = ']';
    close[cl] = 0;

    /* Read until close or EOF */
    size_t cap = 64;
    char  *buf = lv_alloc(cap);
    size_t len = 0;
    for (;;) {
        if (peek_ch(lx) == '\0') {
            /* unterminated long string */
            lv_free(buf);
            return 0;
        }
        if (peek_ch(lx) == ']') {
            int match = 1;
            for (int i = 0; i < cl; i++) {
                if (lx->src[lx->pos + i] != close[i]) { match = 0; break; }
            }
            if (match) {
                for (int i = 0; i < cl; i++) next_ch(lx);
                break;
            }
        }
        if (len + 1 >= cap) { cap *= 2; buf = lv_realloc(buf, cap); }
        buf[len++] = next_ch(lx);
    }
    buf[len] = 0;
    tok->str     = buf;
    tok->str_len = len;
    return 1;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a numeric literal. Handles:
 *   decimal int:   42, 0
 *   hex int:       0x1A, 0Xff
 *   decimal float: 3.14, .5, 1., 1e10, 1.5E-3
 *   hex float:     0x1.8p1, 0XA.BCp+4
 * Returns 1 if the literal is a float, 0 if integer. */
static void read_number(lv_lexer_t *lx) {
    size_t start = lx->pos;
    bool is_float = false;
    bool is_hex   = false;

    if (peek_ch(lx) == '0' && (peek_ch2(lx) == 'x' || peek_ch2(lx) == 'X')) {
        is_hex = true;
        next_ch(lx); next_ch(lx);
        while (isxdigit((unsigned char)peek_ch(lx))) next_ch(lx);
        if (peek_ch(lx) == '.') {
            is_float = true;
            next_ch(lx);
            while (isxdigit((unsigned char)peek_ch(lx))) next_ch(lx);
        }
        if (peek_ch(lx) == 'p' || peek_ch(lx) == 'P') {
            is_float = true;
            next_ch(lx);
            if (peek_ch(lx) == '+' || peek_ch(lx) == '-') next_ch(lx);
            while (isdigit((unsigned char)peek_ch(lx))) next_ch(lx);
        }
    } else {
        while (isdigit((unsigned char)peek_ch(lx))) next_ch(lx);
        if (peek_ch(lx) == '.') {
            is_float = true;
            next_ch(lx);
            while (isdigit((unsigned char)peek_ch(lx))) next_ch(lx);
        }
        if (peek_ch(lx) == 'e' || peek_ch(lx) == 'E') {
            is_float = true;
            next_ch(lx);
            if (peek_ch(lx) == '+' || peek_ch(lx) == '-') next_ch(lx);
            while (isdigit((unsigned char)peek_ch(lx))) next_ch(lx);
        }
    }

    size_t len = lx->pos - start;
    char *buf = lv_strndup(lx->src + start, len);

    set_token(lx, LV_TOK_NUMBER);
    lx->cur.is_float = is_float;
    if (is_float) {
        if (is_hex) {
            lx->cur.fval = strtod(buf, NULL);
        } else {
            lx->cur.fval = strtod(buf, NULL);
        }
    } else {
        if (is_hex) {
            lx->cur.ival = (int64_t)strtoull(buf, NULL, 16);
        } else {
            lx->cur.ival = (int64_t)strtoll(buf, NULL, 10);
        }
    }
    lv_free(buf);
}

/* Read a \-escape sequence inside a quoted string. *pos is just past '\\'.
 * Appends the decoded byte(s) to buf. */
static void read_escape(lv_lexer_t *lx, char **buf, size_t *len, size_t *cap) {
    char c = next_ch(lx);
    char out;
    switch (c) {
    case 'a': out = '\a'; break;
    case 'b': out = '\b'; break;
    case 'f': out = '\f'; break;
    case 'n': out = '\n'; break;
    case 'r': out = '\r'; break;
    case 't': out = '\t'; break;
    case 'v': out = '\v'; break;
    case '\\': out = '\\'; break;
    case '"': out = '"'; break;
    case '\'': out = '\''; break;
    case '\n': out = '\n'; break;
    case '\r':
        out = '\n';
        if (peek_ch(lx) == '\n') next_ch(lx);
        break;
    case 'x': {
        int v = 0;
        for (int i = 0; i < 2; i++) {
            int h = hex_value(peek_ch(lx));
            if (h < 0) break;
            next_ch(lx);
            v = (v << 4) | h;
        }
        out = (char)v;
        break;
    }
    case 'z':
        /* skip following whitespace */
        while (isspace((unsigned char)peek_ch(lx))) next_ch(lx);
        return;
    default:
        if (c >= '0' && c <= '9') {
            int v = c - '0';
            for (int i = 0; i < 2; i++) {
                char d = peek_ch(lx);
                if (d >= '0' && d <= '9') { v = v * 10 + (d - '0'); next_ch(lx); }
                else break;
            }
            out = (char)v;
        } else {
            /* unknown escape — keep the char literally */
            out = c;
        }
    }
    if (*len + 1 >= *cap) { *cap *= 2; *buf = lv_realloc(*buf, *cap); }
    (*buf)[(*len)++] = out;
}

static void read_quoted_string(lv_lexer_t *lx, char quote) {
    next_ch(lx); /* consume opening quote */
    size_t cap = 16;
    char  *buf = lv_alloc(cap);
    size_t len = 0;
    for (;;) {
        char c = peek_ch(lx);
        if (c == '\0' || c == '\n') {
            /* unterminated string */
            break;
        }
        if (c == quote) {
            next_ch(lx);
            break;
        }
        if (c == '\\') {
            next_ch(lx); /* consume backslash */
            read_escape(lx, &buf, &len, &cap);
            continue;
        }
        if (len + 1 >= cap) { cap *= 2; buf = lv_realloc(buf, cap); }
        buf[len++] = next_ch(lx);
    }
    buf[len] = 0;
    set_token(lx, LV_TOK_STRING);
    lx->cur.str     = buf;
    lx->cur.str_len = len;
}

void lv_lexer_next(lv_lexer_t *lx) {
    skip_ws_and_comments(lx);

    char c = peek_ch(lx);
    if (c == '\0') {
        set_token(lx, LV_TOK_EOF);
        return;
    }

    /* Identifier / keyword */
    if (is_ident_start(c)) {
        read_identifier(lx);
        return;
    }

    /* Number */
    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)peek_ch2(lx)))) {
        read_number(lx);
        return;
    }

    /* String */
    if (c == '"' || c == '\'') {
        read_quoted_string(lx, c);
        return;
    }
    if (c == '[') {
        /* Could be a long string [[ or [=[ */
        size_t save = lx->pos;
        int line = lx->line, col = lx->column;
        next_ch(lx); /* consume [ */
        /* try long string */
        lv_token_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (read_long_string(lx, &tmp)) {
            lv_token_fini(&lx->prev);
            lx->prev = lx->cur;
            tmp.kind   = LV_TOK_STRING;
            tmp.line   = line;
            tmp.column = col;
            lx->cur = tmp;
            return;
        }
        /* Not a long string — rewind and emit [ token */
        lx->pos = save;
        lx->line = line;
        lx->column = col;
        next_ch(lx);
        set_token(lx, LV_TOK_LBRACKET);
        return;
    }

    /* Operators and punctuation */
    next_ch(lx); /* consume c */
    switch (c) {
    case '+': set_token(lx, LV_TOK_PLUS); return;
    case '-': set_token(lx, LV_TOK_MINUS); return;
    case '*': set_token(lx, LV_TOK_STAR); return;
    case '/':
        if (peek_ch(lx) == '/') { next_ch(lx); set_token(lx, LV_TOK_DSLASH); return; }
        set_token(lx, LV_TOK_SLASH); return;
    case '%': set_token(lx, LV_TOK_PERCENT); return;
    case '^': set_token(lx, LV_TOK_CARET); return;
    case '#': set_token(lx, LV_TOK_HASH); return;
    case '&': set_token(lx, LV_TOK_AMP); return;
    case '|': set_token(lx, LV_TOK_PIPE); return;
    case '~':
        if (peek_ch(lx) == '=') { next_ch(lx); set_token(lx, LV_TOK_NOTEQ); return; }
        set_token(lx, LV_TOK_TILDE); return;
    case '<':
        if (peek_ch(lx) == '<') { next_ch(lx); set_token(lx, LV_TOK_DLT); return; }
        if (peek_ch(lx) == '=') { next_ch(lx); set_token(lx, LV_TOK_LE); return; }
        set_token(lx, LV_TOK_LT); return;
    case '>':
        if (peek_ch(lx) == '>') { next_ch(lx); set_token(lx, LV_TOK_DGT); return; }
        if (peek_ch(lx) == '=') { next_ch(lx); set_token(lx, LV_TOK_GE); return; }
        set_token(lx, LV_TOK_GT); return;
    case '=':
        if (peek_ch(lx) == '=') { next_ch(lx); set_token(lx, LV_TOK_EQEQ); return; }
        set_token(lx, LV_TOK_ASSIGN); return;
    case '(': set_token(lx, LV_TOK_LPAREN); return;
    case ')': set_token(lx, LV_TOK_RPAREN); return;
    case '{': set_token(lx, LV_TOK_LBRACE); return;
    case '}': set_token(lx, LV_TOK_RBRACE); return;
    case ']': set_token(lx, LV_TOK_RBRACKET); return;
    case ';': set_token(lx, LV_TOK_SEMI); return;
    case ':':
        if (peek_ch(lx) == ':') { next_ch(lx); set_token(lx, LV_TOK_DCOLON); return; }
        set_token(lx, LV_TOK_COLON); return;
    case ',': set_token(lx, LV_TOK_COMMA); return;
    case '.':
        if (peek_ch(lx) == '.') {
            next_ch(lx);
            if (peek_ch(lx) == '.') { next_ch(lx); set_token(lx, LV_TOK_DOTS); return; }
            set_token(lx, LV_TOK_CONCAT); return;
        }
        set_token(lx, LV_TOK_DOT); return;
    }

    set_token(lx, LV_TOK_EOF);
}

const lv_token_t *lv_lexer_peek(const lv_lexer_t *lx) {
    return &lx->cur;
}
