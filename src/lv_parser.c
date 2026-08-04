/* lv_parser.c — Recursive-descent parser for Lua 5.4 syntax. */

#include "lv_parser.h"
#include <stdarg.h>

/* ---- Error reporting ---- */
static void parse_error(lv_parser_t *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const lv_token_t *t = lv_lexer_peek(&p->lx);
    if (p->source_name) {
        char full[768];
        snprintf(full, sizeof(full), "%s:%d: %s (near %s)",
                 p->source_name, t ? t->line : 0, buf,
                 t ? lv_token_kind_name(t->kind) : "<eof>");
        if (p->last_error) lv_free(p->last_error);
        p->last_error = lv_strdup(full);
    } else {
        if (p->last_error) lv_free(p->last_error);
        p->last_error = lv_strdup(buf);
    }
    p->error_count++;
}

/* ---- Token helpers ---- */
static const lv_token_t *cur(lv_parser_t *p) {
    return lv_lexer_peek(&p->lx);
}

static lv_token_kind_t cur_kind(lv_parser_t *p) {
    return cur(p)->kind;
}

static int accept(lv_parser_t *p, lv_token_kind_t k) {
    if (cur_kind(p) == k) { lv_lexer_next(&p->lx); return 1; }
    return 0;
}

static void expect(lv_parser_t *p, lv_token_kind_t k, const char *what) {
    if (!accept(p, k)) {
        parse_error(p, "expected %s", what);
    }
}

static int cur_line(lv_parser_t *p) { return cur(p)->line; }

/* ---- Forward declarations ---- */
static lv_node_t *parse_expr(lv_parser_t *p);
static lv_node_t *parse_block(lv_parser_t *p);
static lv_node_t *parse_statement(lv_parser_t *p);
static lv_func_proto_t *parse_function_body(lv_parser_t *p, const char *name);
static lv_node_t *parse_suffixed_expr(lv_parser_t *p);
static lv_node_t *parse_primary_expr(lv_parser_t *p);
static lv_node_t *parse_simple_expr(lv_parser_t *p);
static lv_node_t *parse_subexpr(lv_parser_t *p, int limit);
static bool peek_after_name_is_assign(lv_parser_t *p);

/* ---- Operator precedence (Lua 5.4 reference manual, §3.4.8) ----
 * Returns -1 for non-binary-operator tokens. */
typedef struct { lv_token_kind_t t; lv_binop_t op; int prec; bool right_assoc; } op_info_t;
static const op_info_t k_binops[] = {
    {LV_TOK_OR,      LV_BIN_OR,    1, false},
    {LV_TOK_AND,     LV_BIN_AND,   2, false},
    {LV_TOK_LT,      LV_BIN_LT,    3, false},
    {LV_TOK_GT,      LV_BIN_GT,    3, false},
    {LV_TOK_LE,      LV_BIN_LE,    3, false},
    {LV_TOK_GE,      LV_BIN_GE,    3, false},
    {LV_TOK_EQEQ,    LV_BIN_EQ,    3, false},
    {LV_TOK_NOTEQ,   LV_BIN_NE,    3, false},
    {LV_TOK_PIPE,    LV_BIN_BOR,   4, false},
    {LV_TOK_TILDE,   LV_BIN_BXOR,  5, false},
    {LV_TOK_AMP,     LV_BIN_BAND,  6, false},
    {LV_TOK_DLT,     LV_BIN_SHL,   7, false},
    {LV_TOK_DGT,     LV_BIN_SHR,   7, false},
    {LV_TOK_DSLASH,  LV_BIN_IDIV,  8, false},
    {LV_TOK_PLUS,    LV_BIN_ADD,   9, false},
    {LV_TOK_MINUS,   LV_BIN_SUB,   9, false},
    {LV_TOK_STAR,    LV_BIN_MUL,  10, false},
    {LV_TOK_SLASH,   LV_BIN_DIV,  10, false},
    {LV_TOK_PERCENT, LV_BIN_MOD,  10, false},
    {LV_TOK_CONCAT,  LV_BIN_CONCAT, 11, true},
    {LV_TOK_CARET,   LV_BIN_POW,    12, true},
};

static const op_info_t *find_binop(lv_token_kind_t k) {
    for (size_t i = 0; i < sizeof(k_binops) / sizeof(k_binops[0]); i++) {
        if (k_binops[i].t == k) return &k_binops[i];
    }
    return NULL;
}

/* ---- Parser init/fini ---- */
void lv_parser_init(lv_parser_t *p, const char *src, size_t src_len,
                    const char *source_name) {
    memset(p, 0, sizeof(*p));
    lv_lexer_init(&p->lx, src, src_len);
    p->source_name = source_name ? source_name : "<input>";
    /* Prime the first token */
    lv_lexer_next(&p->lx);
}

void lv_parser_fini(lv_parser_t *p) {
    lv_lexer_fini(&p->lx);
    if (p->last_error) lv_free(p->last_error);
}

/* ---- Block / statement list ----
 * block ::= {stat} [retstat]
 * stat  ::= ';' |
 *           varlist '=' explist |
 *           functioncall |
 *           label |
 *           break |
 *           goto Name |
 *           do block end |
 *           while exp do block end |
 *           repeat block until exp |
 *           if exp then block {elseif exp then block} [else block] end |
 *           for Name '=' exp ',' exp [',' exp] do block end |
 *           for namelist in explist do block end |
 *           function funcname funcbody |
 *           local function Name funcbody |
 *           local attnamelist ['=' explist]
 */

/* Collect a sequence of statements until an END/ELSE/ELSEIF/UNTIL/EOF
 * terminator. Returns a LV_NODE_DO node wrapping the body (the parser
 * uses LV_NODE_DO as a generic "block" container). */
static lv_node_t *parse_block(lv_parser_t *p) {
    int line = cur_line(p);
    lv_node_t **body = NULL;
    int nbody = 0, cap = 0;

    for (;;) {
        lv_token_kind_t k = cur_kind(p);
        if (k == LV_TOK_END || k == LV_TOK_ELSE || k == LV_TOK_ELSEIF ||
            k == LV_TOK_UNTIL || k == LV_TOK_EOF) {
            break;
        }
        /* retstat */
        if (k == LV_TOK_RETURN) {
            int rl = cur_line(p);
            lv_lexer_next(&p->lx);
            lv_node_t **vals = NULL;
            int nvals = 0, vcap = 0;
            /* retstat ::= return [explist] [';'] */
            k = cur_kind(p);
            if (k != LV_TOK_END && k != LV_TOK_ELSE && k != LV_TOK_ELSEIF &&
                k != LV_TOK_UNTIL && k != LV_TOK_EOF && k != LV_TOK_SEMI) {
                do {
                    lv_node_t *e = parse_expr(p);
                    if (!e) { if (vals) { for (int i=0;i<nvals;i++) lv_node_free(vals[i]); lv_free(vals); } return NULL; }
                    if (nvals + 1 > vcap) { vcap = vcap ? vcap * 2 : 4; vals = lv_realloc(vals, sizeof(lv_node_t *) * vcap); }
                    vals[nvals++] = e;
                } while (accept(p, LV_TOK_COMMA));
            }
            accept(p, LV_TOK_SEMI);
            if (nvals + 1 > cap) { cap = cap ? cap * 2 : 4; body = lv_realloc(body, sizeof(lv_node_t *) * cap); }
            body[nbody++] = lv_node_new_return(rl, vals, nvals);
            if (vals) lv_free(vals);
            break;
        }

        lv_node_t *st = parse_statement(p);
        if (!st) {
            /* On error, free what we have and bail */
            for (int i = 0; i < nbody; i++) lv_node_free(body[i]);
            lv_free(body);
            return NULL;
        }
        if (nbody + 1 > cap) { cap = cap ? cap * 2 : 8; body = lv_realloc(body, sizeof(lv_node_t *) * cap); }
        body[nbody++] = st;
    }

    return lv_node_new_do(line, body, nbody);
}

/* Parse a statement. Returns NULL on error. */
static lv_node_t *parse_statement(lv_parser_t *p) {
    int line = cur_line(p);
    lv_token_kind_t k = cur_kind(p);

    /* ';' (empty statement) — consume and return an empty DO node. */
    if (k == LV_TOK_SEMI) { lv_lexer_next(&p->lx); return lv_node_new_do(line, NULL, 0); }

    /* break */
    if (k == LV_TOK_BREAK) {
        lv_lexer_next(&p->lx);
        return lv_node_new_break(line);
    }

    /* goto Name */
    if (k == LV_TOK_GOTO) {
        lv_lexer_next(&p->lx);
        if (cur_kind(p) != LV_TOK_NAME) {
            parse_error(p, "expected name after 'goto'");
            return NULL;
        }
        const lv_token_t *t = cur(p);
        lv_node_t *n = lv_node_new_goto(line, t->name);
        lv_lexer_next(&p->lx);
        return n;
    }

    /* ::label:: */
    if (k == LV_TOK_DCOLON) {
        lv_lexer_next(&p->lx);
        if (cur_kind(p) != LV_TOK_NAME) {
            parse_error(p, "expected name after '::'");
            return NULL;
        }
        const lv_token_t *t = cur(p);
        lv_node_t *n = lv_node_new_label(line, t->name);
        lv_lexer_next(&p->lx);
        expect(p, LV_TOK_DCOLON, "'::'");
        return n;
    }

    /* do block end */
    if (k == LV_TOK_DO) {
        lv_lexer_next(&p->lx);
        lv_node_t *body = parse_block(p);
        expect(p, LV_TOK_END, "'end'");
        return body;
    }

    /* while exp do block end */
    if (k == LV_TOK_WHILE) {
        lv_lexer_next(&p->lx);
        lv_node_t *cond = parse_expr(p);
        if (!cond) return NULL;
        expect(p, LV_TOK_DO, "'do'");
        lv_node_t *body = parse_block(p);
        expect(p, LV_TOK_END, "'end'");
        /* Unwrap body LV_NODE_DO into children array */
        lv_node_t *n = lv_node_new_while(line, cond,
                                          body ? body->children : NULL,
                                          body ? body->nchildren : 0);
        if (body) {
            /* children ownership transferred; free the wrapper only */
            lv_free(body->children);
            body->children = NULL;
            body->nchildren = 0;
            lv_node_free(body);
        }
        return n;
    }

    /* repeat block until exp */
    if (k == LV_TOK_REPEAT) {
        lv_lexer_next(&p->lx);
        lv_node_t *body = parse_block(p);
        expect(p, LV_TOK_UNTIL, "'until'");
        lv_node_t *cond = parse_expr(p);
        if (!cond) { lv_node_free(body); return NULL; }
        lv_node_t *n = lv_node_new_repeat(line,
                                           body ? body->children : NULL,
                                           body ? body->nchildren : 0,
                                           cond);
        if (body) {
            lv_free(body->children);
            body->children = NULL;
            body->nchildren = 0;
            lv_node_free(body);
        }
        return n;
    }

    /* if exp then block {elseif exp then block} [else block] end */
    if (k == LV_TOK_IF) {
        lv_lexer_next(&p->lx);
        lv_node_t *root = lv_node_new_if(line);
        for (;;) {
            lv_node_t *cond = parse_expr(p);
            if (!cond) { lv_node_free(root); return NULL; }
            expect(p, LV_TOK_THEN, "'then'");
            lv_node_t *body = parse_block(p);
            lv_node_append_child(root, cond);
            if (body) {
                /* Append a DO node as a marker for "then-body" */
                lv_node_append_child(root, body);
            } else {
                lv_node_append_child(root, lv_node_new_do(line, NULL, 0));
            }
            if (accept(p, LV_TOK_ELSEIF)) continue;
            break;
        }
        if (accept(p, LV_TOK_ELSE)) {
            lv_node_t *eb = parse_block(p);
            lv_node_append_child(root, eb ? eb : lv_node_new_do(line, NULL, 0));
        } else {
            lv_node_append_child(root, lv_node_new_do(line, NULL, 0));
        }
        expect(p, LV_TOK_END, "'end'");
        return root;
    }

    /* for Name '=' exp ',' exp [',' exp] do block end
     * | for namelist in explist do block end */
    if (k == LV_TOK_FOR) {
        lv_lexer_next(&p->lx);
        if (cur_kind(p) != LV_TOK_NAME) {
            parse_error(p, "expected name after 'for'");
            return NULL;
        }
        const lv_token_t *first = cur(p);
        char *first_name = lv_strdup(first->name);
        lv_lexer_next(&p->lx);

        if (cur_kind(p) == LV_TOK_ASSIGN) {
            /* numeric for */
            lv_lexer_next(&p->lx);
            lv_node_t *init = parse_expr(p);
            expect(p, LV_TOK_COMMA, "','");
            lv_node_t *limit = parse_expr(p);
            lv_node_t *step = NULL;
            if (accept(p, LV_TOK_COMMA)) step = parse_expr(p);
            expect(p, LV_TOK_DO, "'do'");
            lv_node_t *body = parse_block(p);
            expect(p, LV_TOK_END, "'end'");
            lv_node_t *n = lv_node_new_for_num(line, first_name,
                                                init, limit, step,
                                                body ? body->children : NULL,
                                                body ? body->nchildren : 0);
            if (body) {
                lv_free(body->children);
                body->children = NULL;
                body->nchildren = 0;
                lv_node_free(body);
            }
            lv_free(first_name);
            return n;
        } else {
            /* generic for: collect namelist */
            char *names[16];
            int nnames = 0;
            names[nnames++] = first_name;
            while (accept(p, LV_TOK_COMMA)) {
                if (cur_kind(p) != LV_TOK_NAME || nnames >= 16) {
                    parse_error(p, "expected name in for-loop variable list");
                    for (int i = 0; i < nnames; i++) lv_free(names[i]);
                    return NULL;
                }
                names[nnames++] = lv_strdup(cur(p)->name);
                lv_lexer_next(&p->lx);
            }
            expect(p, LV_TOK_IN, "'in'");
            /* explist */
            lv_node_t *exprs[16];
            int nexprs = 0;
            do {
                if (nexprs >= 16) { parse_error(p, "too many expressions"); return NULL; }
                exprs[nexprs] = parse_expr(p);
                if (!exprs[nexprs]) return NULL;
                nexprs++;
            } while (accept(p, LV_TOK_COMMA));
            expect(p, LV_TOK_DO, "'do'");
            lv_node_t *body = parse_block(p);
            expect(p, LV_TOK_END, "'end'");
            lv_node_t *n = lv_node_new_for_in(line, names, nnames,
                                                exprs, nexprs,
                                                body ? body->children : NULL,
                                                body ? body->nchildren : 0);
            if (body) {
                lv_free(body->children);
                body->children = NULL;
                body->nchildren = 0;
                lv_node_free(body);
            }
            for (int i = 0; i < nnames; i++) lv_free(names[i]);
            return n;
        }
    }

    /* function funcname funcbody */
    if (k == LV_TOK_FUNCTION) {
        lv_lexer_next(&p->lx);
        /* funcname ::= Name {'.' Name} [':' Name] */
        if (cur_kind(p) != LV_TOK_NAME) {
            parse_error(p, "expected function name");
            return NULL;
        }
        /* For MVP: support only `function name(...)` and `local function name(...)`.
         * Full dotted/colon names parsed but lowered as: build a chain of
         * table index assignments. */
        char name[256];
        snprintf(name, sizeof(name), "%s", cur(p)->name);
        lv_lexer_next(&p->lx);
        /* TODO: dotted funcname. For now we just take the first name. */
        lv_func_proto_t *proto = parse_function_body(p, name);
        if (!proto) return NULL;
        lv_node_t *fnode = lv_node_new_function(line, proto);
        /* Wrap as: name = function ... end  (assignment to global) */
        lv_node_t *targets[1] = { lv_node_new_name(line, name) };
        lv_node_t *vals[1]    = { fnode };
        return lv_node_new_assign(line, targets, 1, vals, 1);
    }

    /* local function Name funcbody | local namelist ['=' explist] */
    if (k == LV_TOK_LOCAL) {
        lv_lexer_next(&p->lx);
        if (accept(p, LV_TOK_FUNCTION)) {
            if (cur_kind(p) != LV_TOK_NAME) {
                parse_error(p, "expected name after 'local function'");
                return NULL;
            }
            const lv_token_t *t = cur(p);
            char *fname = lv_strdup(t->name);
            int fl = t->line;
            lv_lexer_next(&p->lx);
            lv_func_proto_t *proto = parse_function_body(p, fname);
            if (!proto) { lv_free(fname); return NULL; }
            lv_node_t *fnode = lv_node_new_function(fl, proto);
            /* local function f(...) ... end  ==>  local f = (function ...) */
            char *names[1] = { fname };
            lv_node_t *vals[1] = { fnode };
            lv_node_t *n = lv_node_new_local(fl, names, 1, vals, 1);
            lv_free(fname);
            return n;
        }
        /* local namelist ['=' explist] */
        char *names[64];
        int nnames = 0;
        for (;;) {
            if (cur_kind(p) != LV_TOK_NAME || nnames >= 64) {
                parse_error(p, "expected name in local declaration");
                for (int i = 0; i < nnames; i++) lv_free(names[i]);
                return NULL;
            }
            names[nnames++] = lv_strdup(cur(p)->name);
            lv_lexer_next(&p->lx);
            /* attribute: <const>, <close> -- skip for MVP */
            if (accept(p, LV_TOK_LT)) {
                if (cur_kind(p) == LV_TOK_NAME) lv_lexer_next(&p->lx);
                expect(p, LV_TOK_GT, "'>'");
            }
            if (!accept(p, LV_TOK_COMMA)) break;
        }
        lv_node_t **vals = NULL;
        int nvals = 0;
        if (accept(p, LV_TOK_ASSIGN)) {
            do {
                lv_node_t *e = parse_expr(p);
                if (!e) { for (int i=0;i<nnames;i++) lv_free(names[i]); return NULL; }
                vals = lv_realloc(vals, sizeof(lv_node_t *) * (nvals + 1));
                vals[nvals++] = e;
            } while (accept(p, LV_TOK_COMMA));
        }
        lv_node_t *n = lv_node_new_local(line, names, nnames, vals, nvals);
        for (int i = 0; i < nnames; i++) lv_free(names[i]);
        if (vals) lv_free(vals);
        return n;
    }

    /* Otherwise: either a function call or an assignment.
     * Parse a "suffixed expression"; if followed by '=' or ',', it's
     * an assignment; otherwise it must be a call (used as a statement). */
    lv_node_t *first = parse_suffixed_expr(p);
    if (!first) return NULL;

    if (cur_kind(p) == LV_TOK_ASSIGN || cur_kind(p) == LV_TOK_COMMA) {
        /* assignment */
        lv_node_t *targets[64];
        int ntargets = 0;
        targets[ntargets++] = first;
        while (accept(p, LV_TOK_COMMA)) {
            lv_node_t *t = parse_suffixed_expr(p);
            if (!t) { for (int i=0;i<ntargets;i++) lv_node_free(targets[i]); return NULL; }
            if (ntargets >= 64) { parse_error(p, "too many assignment targets"); lv_node_free(t); return NULL; }
            targets[ntargets++] = t;
        }
        expect(p, LV_TOK_ASSIGN, "'='");
        lv_node_t *vals[64];
        int nvals = 0;
        do {
            lv_node_t *e = parse_expr(p);
            if (!e) { for (int i=0;i<ntargets;i++) lv_node_free(targets[i]); return NULL; }
            if (nvals >= 64) { parse_error(p, "too many values"); lv_node_free(e); return NULL; }
            vals[nvals++] = e;
        } while (accept(p, LV_TOK_COMMA));
        return lv_node_new_assign(line, targets, ntargets, vals, nvals);
    }

    /* Must be a function call */
    if (first->kind != LV_NODE_CALL && first->kind != LV_NODE_METHOD_CALL) {
        parse_error(p, "syntax error: expected statement");
        lv_node_free(first);
        return NULL;
    }
    return lv_node_new_call_stmt(line, first);
}

/* ---- Function body ----
 * funcbody ::= '(' [parlist] ')' block end
 * parlist  ::= namelist [',' '...'] | '...'
 */
static lv_func_proto_t *parse_function_body(lv_parser_t *p, const char *name) {
    expect(p, LV_TOK_LPAREN, "'('");
    char *params[64];
    int nparams = 0;
    bool is_vararg = false;
    if (cur_kind(p) != LV_TOK_RPAREN) {
        if (cur_kind(p) == LV_TOK_DOTS) {
            lv_lexer_next(&p->lx);
            is_vararg = true;
        } else {
            for (;;) {
                if (cur_kind(p) != LV_TOK_NAME || nparams >= 64) {
                    parse_error(p, "expected parameter name");
                    for (int i=0;i<nparams;i++) lv_free(params[i]);
                    return NULL;
                }
                params[nparams++] = lv_strdup(cur(p)->name);
                lv_lexer_next(&p->lx);
                if (accept(p, LV_TOK_COMMA)) {
                    if (cur_kind(p) == LV_TOK_DOTS) {
                        lv_lexer_next(&p->lx);
                        is_vararg = true;
                        break;
                    }
                    continue;
                }
                break;
            }
        }
    }
    expect(p, LV_TOK_RPAREN, "')'");

    int body_line = cur_line(p);
    lv_node_t *body = parse_block(p);
    expect(p, LV_TOK_END, "'end'");

    lv_func_proto_t *proto = lv_alloc(sizeof(*proto));
    memset(proto, 0, sizeof(*proto));
    proto->name = name ? lv_strdup(name) : NULL;
    if (nparams > 0) {
        proto->params = lv_alloc(sizeof(char *) * nparams);
        for (int i = 0; i < nparams; i++) proto->params[i] = params[i];
    }
    proto->nparams  = nparams;
    proto->is_vararg = is_vararg;
    if (body) {
        proto->body = body->children;
        proto->nbody = body->nchildren;
        body->children = NULL;
        body->nchildren = 0;
        lv_node_free(body);
    }
    proto->line = body_line;
    return proto;
}

/* ---- Expressions ----
 * Grammar (Lua 5.4 §3.4):
 *   exp ::= nil | true | false | Numeral | LiteralString | '...' | functiondef |
 *           prefixexp | tableconstructor | exp binop exp | unop exp
 *
 *   prefixexp ::= var | functioncall | '(' exp ')'
 *   var       ::= Name | prefixexp '[' exp ']' | prefixexp '.' Name
 *   functioncall ::= prefixexp args | prefixexp ':' Name args
 *   args      ::= '(' [explist] ')' | tableconstructor | LiteralString
 *   functiondef ::= function funcbody
 *   tableconstructor ::= '{' [fieldlist] '}'
 *   fieldlist ::= field {fieldsep field} [fieldsep]
 *   field     ::= '[' exp ']' '=' exp | Name '=' exp | exp
 *   fieldsep  ::= ',' | ';'
 */

static lv_node_t *parse_table_constructor(lv_parser_t *p) {
    int line = cur_line(p);
    lv_lexer_next(&p->lx); /* consume { */
    lv_node_t *tbl = lv_node_new_table(line);
    while (cur_kind(p) != LV_TOK_RBRACE && cur_kind(p) != LV_TOK_EOF) {
        lv_node_t *entry = NULL;
        if (cur_kind(p) == LV_TOK_LBRACKET) {
            lv_lexer_next(&p->lx);
            lv_node_t *k = parse_expr(p);
            expect(p, LV_TOK_RBRACKET, "']'");
            expect(p, LV_TOK_ASSIGN, "'='");
            lv_node_t *v = parse_expr(p);
            /* Represent as a kv-pair: use INDEX-style 2-child node */
            entry = lv_node_new_index(line, k, v);
            entry->kind = LV_NODE_TABLE; /* mark as kv-entry via kind hack */
            /* Actually, we need a distinct representation. Use a TABLE node
             * with 2 children: key and value. The parent TABLE collects
             * entries; each entry is either a single value (positional)
             * or a 2-child TABLE node (kv). */
            /* Re-build: create a fresh TABLE node with k and v as children */
            lv_node_free(entry);
            lv_node_t *kv = lv_node_new_table(line);
            lv_node_append_child(kv, k);
            lv_node_append_child(kv, v);
            entry = kv;
        } else if (cur_kind(p) == LV_TOK_NAME && peek_after_name_is_assign(p)) {
            /* Name '=' exp */
            const lv_token_t *t = cur(p);
            lv_node_t *k = lv_node_new_string(line, t->name, strlen(t->name));
            lv_lexer_next(&p->lx);
            expect(p, LV_TOK_ASSIGN, "'='");
            lv_node_t *v = parse_expr(p);
            lv_node_t *kv = lv_node_new_table(line);
            lv_node_append_child(kv, k);
            lv_node_append_child(kv, v);
            entry = kv;
        } else {
            /* positional value */
            entry = parse_expr(p);
        }
        if (!entry) { lv_node_free(tbl); return NULL; }
        lv_node_append_child(tbl, entry);
        /* fieldsep */
        if (!accept(p, LV_TOK_COMMA) && !accept(p, LV_TOK_SEMI)) break;
    }
    expect(p, LV_TOK_RBRACE, "'}'");
    return tbl;
}

/* We implement it via a stashed copy of the lexer's cur token + pos. */
static bool peek_after_name_is_assign(lv_parser_t *p) {
    /* We can't easily peek 2 tokens ahead without consuming. The cleanest
     * fix is to make the lexer support 1-token lookahead. For MVP we just
     * always parse Name and let the table-field logic deal with the
     * ambiguity: if after consuming Name we see '=', it was a kv-pair;
     * otherwise it's a positional value and we treat Name as the value. */
    /* To avoid breaking the parse, we save lexer state and try consuming. */
    /* This is only called when cur is NAME; let's actually consume and
     * see, then restore if needed. The lexer doesn't support undo, so we
     * implement a tiny lookahead by saving cur. */
    /* Strategy: peek next token; lexer's cur is NAME, we need to see
     * the one after. We'll save the cur token, call next, peek, restore. */
    /* But lv_lexer_next moves cur->prev and frees cur strings. The
     * saved token would need to be restored manually. */
    /* Simplest correct approach: implement true 2-token lookahead in the
     * lexer. For MVP we use a heuristic: if next token after NAME is '=',
     * it's a kv-pair. We can detect this by peeking at the raw source:
     * skip whitespace from the current position. */
    /* Actually, let's just implement it via a temporary lexer next/restore. */
    /* Save the current token (deep copy). */
    lv_token_t saved;
    memcpy(&saved, &p->lx.cur, sizeof(saved));
    /* saved.str and saved.name are owned by lx.cur; we duplicate them
     * so they survive a call to lv_lexer_next. */
    if (saved.str)  saved.str  = lv_strndup(saved.str, saved.str_len);
    if (saved.name) saved.name = lv_strdup(saved.name);
    /* Also save pos/line/column */
    size_t save_pos = p->lx.pos;
    int save_line = p->lx.line;
    int save_col  = p->lx.column;

    lv_lexer_next(&p->lx);
    bool is_assign = (cur_kind(p) == LV_TOK_ASSIGN);

    /* Restore: put saved back as cur, and rewind pos. But the lexer's
     * cur is now a different token; we need to free it and restore. */
    lv_token_fini(&p->lx.cur);
    p->lx.cur = saved; /* takes ownership of saved.str/name */
    p->lx.pos   = save_pos;
    p->lx.line  = save_line;
    p->lx.column = save_col;
    return is_assign;
}

/* primary expression: literal, name, function, table, (exp) */
static lv_node_t *parse_primary_expr(lv_parser_t *p) {
    int line = cur_line(p);
    lv_token_kind_t k = cur_kind(p);

    switch (k) {
    case LV_TOK_NIL:    lv_lexer_next(&p->lx); return lv_node_new_nil(line);
    case LV_TOK_TRUE:   lv_lexer_next(&p->lx); return lv_node_new_true(line);
    case LV_TOK_FALSE:  lv_lexer_next(&p->lx); return lv_node_new_false(line);
    case LV_TOK_NUMBER: {
        const lv_token_t *t = cur(p);
        lv_node_t *n;
        if (t->is_float) n = lv_node_new_float(line, t->fval);
        else             n = lv_node_new_int(line, t->ival);
        lv_lexer_next(&p->lx);
        return n;
    }
    case LV_TOK_STRING: {
        const lv_token_t *t = cur(p);
        lv_node_t *n = lv_node_new_string(line, t->str, t->str_len);
        lv_lexer_next(&p->lx);
        return n;
    }
    case LV_TOK_DOTS: {
        lv_node_t *n = lv_node_new_dots(line);
        lv_lexer_next(&p->lx);
        return n;
    }
    case LV_TOK_NAME: {
        const lv_token_t *t = cur(p);
        lv_node_t *n = lv_node_new_name(line, t->name);
        lv_lexer_next(&p->lx);
        return n;
    }
    case LV_TOK_LPAREN: {
        lv_lexer_next(&p->lx);
        lv_node_t *e = parse_expr(p);
        expect(p, LV_TOK_RPAREN, "')'");
        return e;
    }
    case LV_TOK_LBRACE: {
        return parse_table_constructor(p);
    }
    case LV_TOK_FUNCTION: {
        lv_lexer_next(&p->lx);
        lv_func_proto_t *proto = parse_function_body(p, NULL);
        if (!proto) return NULL;
        return lv_node_new_function(line, proto);
    }
    default:
        parse_error(p, "unexpected symbol '%s'", lv_token_kind_name(k));
        return NULL;
    }
}

/* suffixed expression: primary { '.' Name | '[' exp ']' | ':' Name args | args } */
static lv_node_t *parse_suffixed_expr(lv_parser_t *p) {
    lv_node_t *e = parse_primary_expr(p);
    if (!e) return NULL;
    for (;;) {
        int line = cur_line(p);
        lv_token_kind_t k = cur_kind(p);
        if (k == LV_TOK_DOT) {
            lv_lexer_next(&p->lx);
            if (cur_kind(p) != LV_TOK_NAME) {
                parse_error(p, "expected name after '.'");
                lv_node_free(e);
                return NULL;
            }
            const lv_token_t *t = cur(p);
            lv_node_t *n = lv_node_new_field(line, e, t->name);
            lv_lexer_next(&p->lx);
            e = n;
        } else if (k == LV_TOK_LBRACKET) {
            lv_lexer_next(&p->lx);
            lv_node_t *idx = parse_expr(p);
            expect(p, LV_TOK_RBRACKET, "']'");
            e = lv_node_new_index(line, e, idx);
        } else if (k == LV_TOK_COLON) {
            lv_lexer_next(&p->lx);
            if (cur_kind(p) != LV_TOK_NAME) {
                parse_error(p, "expected method name after ':'");
                lv_node_free(e);
                return NULL;
            }
            const lv_token_t *t = cur(p);
            char mname[256];
            snprintf(mname, sizeof(mname), "%s", t->name);
            lv_lexer_next(&p->lx);
            /* args */
            lv_node_t **args = NULL;
            int nargs = 0;
            lv_token_kind_t ak = cur_kind(p);
            if (ak == LV_TOK_LPAREN) {
                lv_lexer_next(&p->lx);
                if (cur_kind(p) != LV_TOK_RPAREN) {
                    do {
                        lv_node_t *a = parse_expr(p);
                        if (!a) { lv_node_free(e); return NULL; }
                        args = lv_realloc(args, sizeof(lv_node_t *) * (nargs + 1));
                        args[nargs++] = a;
                    } while (accept(p, LV_TOK_COMMA));
                }
                expect(p, LV_TOK_RPAREN, "')'");
            } else if (ak == LV_TOK_LBRACE) {
                lv_node_t *tbl = parse_table_constructor(p);
                args = lv_alloc(sizeof(lv_node_t *));
                args[nargs++] = tbl;
            } else if (ak == LV_TOK_STRING) {
                const lv_token_t *st = cur(p);
                lv_node_t *s = lv_node_new_string(line, st->str, st->str_len);
                lv_lexer_next(&p->lx);
                args = lv_alloc(sizeof(lv_node_t *));
                args[nargs++] = s;
            } else {
                parse_error(p, "expected function arguments");
                lv_node_free(e);
                return NULL;
            }
            e = lv_node_new_method_call(line, e, mname, args, nargs);
            if (args) lv_free(args);
        } else if (k == LV_TOK_LPAREN || k == LV_TOK_LBRACE || k == LV_TOK_STRING) {
            /* function call args */
            lv_node_t **args = NULL;
            int nargs = 0;
            if (k == LV_TOK_LPAREN) {
                lv_lexer_next(&p->lx);
                if (cur_kind(p) != LV_TOK_RPAREN) {
                    do {
                        lv_node_t *a = parse_expr(p);
                        if (!a) { lv_node_free(e); return NULL; }
                        args = lv_realloc(args, sizeof(lv_node_t *) * (nargs + 1));
                        args[nargs++] = a;
                    } while (accept(p, LV_TOK_COMMA));
                }
                expect(p, LV_TOK_RPAREN, "')'");
            } else if (k == LV_TOK_LBRACE) {
                lv_node_t *tbl = parse_table_constructor(p);
                args = lv_alloc(sizeof(lv_node_t *));
                args[nargs++] = tbl;
            } else { /* STRING */
                const lv_token_t *st = cur(p);
                lv_node_t *s = lv_node_new_string(line, st->str, st->str_len);
                lv_lexer_next(&p->lx);
                args = lv_alloc(sizeof(lv_node_t *));
                args[nargs++] = s;
            }
            e = lv_node_new_call(line, e, args, nargs);
            if (args) lv_free(args);
        } else {
            break;
        }
    }
    return e;
}

/* simple expression: suffixed expr, or constant with no suffix needed */
static lv_node_t *parse_simple_expr(lv_parser_t *p) {
    return parse_suffixed_expr(p);
}

/* unary expression */
static lv_node_t *parse_unop(lv_parser_t *p) {
    int line = cur_line(p);
    lv_token_kind_t k = cur_kind(p);
    lv_unop_t op;
    switch (k) {
    case LV_TOK_MINUS: op = LV_UN_NEG; break;
    case LV_TOK_NOT:   op = LV_UN_NOT; break;
    case LV_TOK_HASH:  op = LV_UN_LEN; break;
    case LV_TOK_TILDE: op = LV_UN_BNOT; break;
    default: return parse_simple_expr(p);
    }
    lv_lexer_next(&p->lx);
    lv_node_t *operand = parse_unop(p); /* unary ops are right-assoc */
    if (!operand) return NULL;
    return lv_node_new_unop(line, op, operand);
}

/* subexpr with precedence climbing */
static lv_node_t *parse_subexpr(lv_parser_t *p, int limit) {
    lv_node_t *left = parse_unop(p);
    if (!left) return NULL;
    for (;;) {
        const op_info_t *op = find_binop(cur_kind(p));
        if (!op || op->prec < limit) break;
        lv_lexer_next(&p->lx);
        int next_limit = op->prec + (op->right_assoc ? 0 : 1);
        lv_node_t *right = parse_subexpr(p, next_limit);
        if (!right) { lv_node_free(left); return NULL; }
        left = lv_node_new_binop(left->line, op->op, left, right);
    }
    return left;
}

static lv_node_t *parse_expr(lv_parser_t *p) {
    return parse_subexpr(p, 0);
}

/* ---- Top-level entry ---- */
lv_node_t *lv_parser_parse(lv_parser_t *p) {
    lv_node_t *block = parse_block(p);
    if (!block) return NULL;
    if (cur_kind(p) != LV_TOK_EOF) {
        parse_error(p, "unexpected trailing input (near %s)",
                    lv_token_kind_name(cur_kind(p)));
        lv_node_free(block);
        return NULL;
    }
    /* Convert the block DO node into a CHUNK node */
    lv_node_t *chunk = lv_node_new_chunk(1, block->children, block->nchildren);
    block->children = NULL;
    block->nchildren = 0;
    lv_node_free(block);
    return chunk;
}

const char *lv_parser_last_error(const lv_parser_t *p) {
    return p ? p->last_error : NULL;
}
