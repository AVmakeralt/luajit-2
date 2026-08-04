/* lv_eval.c — Tree-walking evaluator for Lua function bodies.
 *
 * Walks the AST directly, evaluating expressions to vtx_value_t and
 * executing statements. Used for nested function calls (closures)
 * so that the main chunk can run on VORTEX while still allowing
 * user-defined functions to be called.
 *
 * Control flow (break/return) is implemented via longjmp.
 */

#include "lv_eval.h"
#include "lv_stdlib.h"
#include <setjmp.h>

/* ---- Control-flow signals ---- */
typedef enum {
    LV_SIG_NORMAL = 0,
    LV_SIG_BREAK,
    LV_SIG_RETURN,
} lv_sig_kind_t;

typedef struct lv_sig_buf {
    lv_sig_kind_t  kind;
    vtx_value_t    value;   /* for RETURN */
    jmp_buf        jump;
} lv_sig_buf_t;

/* Thread-local: the current signal handler. NULL at top level. */
static _Thread_local lv_sig_buf_t *g_cur_sig = NULL;
/* Thread-local: the current runtime (for use by scope helpers). */
static _Thread_local lv_runtime_t *g_cur_rt = NULL;

/* ---- Scope management ---- */
lv_eval_scope_t *lv_eval_scope_new(lv_eval_scope_t *parent, lv_table_t *env) {
    lv_eval_scope_t *s = lv_alloc(sizeof(*s));
    s->parent   = parent;
    s->names    = NULL;
    s->vals     = NULL;
    s->count    = 0;
    s->capacity = 0;
    s->env      = env;
    return s;
}

void lv_eval_scope_free(lv_eval_scope_t *scope) {
    /* For MVP, we leak scopes. Closures capture enclosing_scope pointers,
     * and freeing a scope that's still referenced by a closure would
     * cause use-after-free. A future version with GC will track scope
     * ownership properly. */
    (void)scope;
}

vtx_value_t lv_eval_scope_get(lv_eval_scope_t *scope, const char *name) {
    for (lv_eval_scope_t *s = scope; s; s = s->parent) {
        for (int i = s->count - 1; i >= 0; i--) {
            if (strcmp(s->names[i], name) == 0) return s->vals[i];
        }
    }
    if (scope->env && g_cur_rt) {
        return lv_table_get(scope->env,
                            lv_runtime_intern_string(g_cur_rt, name, strlen(name)));
    }
    return VTX_VALUE_NULL;
}

void lv_eval_scope_set(lv_eval_scope_t *scope, const char *name, vtx_value_t val) {
    for (lv_eval_scope_t *s = scope; s; s = s->parent) {
        for (int i = s->count - 1; i >= 0; i--) {
            if (strcmp(s->names[i], name) == 0) {
                s->vals[i] = val;
                return;
            }
        }
    }
    if (scope->env && g_cur_rt) {
        lv_table_set(scope->env,
                     lv_runtime_intern_string(g_cur_rt, name, strlen(name)),
                     val);
    }
}

void lv_eval_scope_declare(lv_eval_scope_t *scope, const char *name, vtx_value_t val) {
    if (scope->count + 1 > scope->capacity) {
        scope->capacity = scope->capacity ? scope->capacity * 2 : 8;
        scope->names = lv_realloc(scope->names, sizeof(char *) * scope->capacity);
        scope->vals  = lv_realloc(scope->vals,  sizeof(vtx_value_t) * scope->capacity);
    }
    scope->names[scope->count] = lv_strdup(name);
    scope->vals[scope->count]  = val;
    scope->count++;
}

/* ---- Forward declarations ---- */
static vtx_value_t eval_expr(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope);
static void eval_stmt(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope);
static void eval_block(lv_runtime_t *rt, lv_node_t *block, lv_eval_scope_t *scope);

/* ---- Arithmetic helper (delegates to stdlib) ---- */
static vtx_value_t eval_binop_arith(lv_runtime_t *rt, lv_binop_t op,
                                     vtx_value_t a, vtx_value_t b) {
    uint16_t fn_id;
    bool swap = false, negate = false;
    switch (op) {
    case LV_BIN_ADD:    fn_id = 120; break;
    case LV_BIN_SUB:    fn_id = 121; break;
    case LV_BIN_MUL:    fn_id = 122; break;
    case LV_BIN_DIV:    fn_id = 123; break;
    case LV_BIN_IDIV:   fn_id = 124; break;
    case LV_BIN_MOD:    fn_id = 125; break;
    case LV_BIN_POW:    fn_id = 126; break;
    case LV_BIN_CONCAT: fn_id = 127; break;
    case LV_BIN_EQ:     fn_id = 130; break;
    case LV_BIN_NE:     fn_id = 130; negate = true; break;
    case LV_BIN_LT:     fn_id = 131; break;
    case LV_BIN_LE:     fn_id = 132; break;
    case LV_BIN_GT:     fn_id = 131; swap = true; break;
    case LV_BIN_GE:     fn_id = 132; swap = true; break;
    case LV_BIN_BAND:   fn_id = 140; break;
    case LV_BIN_BOR:    fn_id = 141; break;
    case LV_BIN_BXOR:   fn_id = 142; break;
    case LV_BIN_SHL:    fn_id = 143; break;
    case LV_BIN_SHR:    fn_id = 144; break;
    default:
        lv_error(rt, "internal: unhandled binop");
        return VTX_VALUE_NULL;
    }
    vtx_value_t argv[2];
    if (swap) { argv[0] = b; argv[1] = a; }
    else      { argv[0] = a; argv[1] = b; }
    vtx_value_t r = lv_stdlib_dispatch(rt, fn_id, argv, 2);
    if (negate) {
        r = vtx_make_bool(!lv_is_truthy(r));
    }
    return r;
}

/* ---- Expression evaluation ---- */
static vtx_value_t eval_expr(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    if (!node) return VTX_VALUE_NULL;
    switch (node->kind) {
    case LV_NODE_NIL:    return VTX_VALUE_NULL;
    case LV_NODE_TRUE:   return VTX_VALUE_TRUE;
    case LV_NODE_FALSE:  return VTX_VALUE_FALSE;
    case LV_NODE_INT:    return vtx_make_smi(node->ival);
    case LV_NODE_FLOAT:  return vtx_make_double(node->fval);
    case LV_NODE_STRING: return lv_runtime_intern_string(rt, node->str, node->str_len);
    case LV_NODE_DOTS:   return VTX_VALUE_NULL; /* MVP: varargs return nil */
    case LV_NODE_NAME:   return lv_eval_scope_get(scope, node->str);
    case LV_NODE_INDEX: {
        vtx_value_t t = eval_expr(rt, node->children[0], scope);
        vtx_value_t k = eval_expr(rt, node->children[1], scope);
        if (lv_is_table(t)) return lv_table_get((lv_table_t *)vtx_heap_ptr(t), k);
        lv_error(rt, "attempt to index a %s value", lv_type_name(t));
        return VTX_VALUE_NULL;
    }
    case LV_NODE_FIELD: {
        vtx_value_t t = eval_expr(rt, node->children[0], scope);
        vtx_value_t k = lv_runtime_intern_string(rt, node->str, strlen(node->str));
        if (lv_is_table(t)) return lv_table_get((lv_table_t *)vtx_heap_ptr(t), k);
        /* String method dispatch: t:method() where t is a string. */
        if (lv_is_string(t)) {
            lv_table_t *strlib = lv_stdlib_string_lib(rt);
            if (strlib) return lv_table_get(strlib, k);
        }
        lv_error(rt, "attempt to index a %s value", lv_type_name(t));
        return VTX_VALUE_NULL;
    }
    case LV_NODE_TABLE: {
        lv_table_t *t = lv_table_new(node->nchildren * 2 + 1);
        int pos = 1;
        for (int i = 0; i < node->nchildren; i++) {
            lv_node_t *e = node->children[i];
            if (e->kind == LV_NODE_TABLE && e->nchildren == 2) {
                vtx_value_t k = eval_expr(rt, e->children[0], scope);
                vtx_value_t v = eval_expr(rt, e->children[1], scope);
                lv_table_set(t, k, v);
            } else {
                vtx_value_t v = eval_expr(rt, e, scope);
                lv_table_set(t, vtx_make_smi(pos++), v);
            }
        }
        return lv_make_table_val(t);
    }
    case LV_NODE_BINOP: {
        if (node->binop == LV_BIN_AND) {
            vtx_value_t l = eval_expr(rt, node->children[0], scope);
            if (!lv_is_truthy(l)) return l;
            return eval_expr(rt, node->children[1], scope);
        }
        if (node->binop == LV_BIN_OR) {
            vtx_value_t l = eval_expr(rt, node->children[0], scope);
            if (lv_is_truthy(l)) return l;
            return eval_expr(rt, node->children[1], scope);
        }
        vtx_value_t l = eval_expr(rt, node->children[0], scope);
        vtx_value_t r = eval_expr(rt, node->children[1], scope);
        return eval_binop_arith(rt, node->binop, l, r);
    }
    case LV_NODE_UNOP: {
        vtx_value_t v = eval_expr(rt, node->children[0], scope);
        switch (node->unop) {
        case LV_UN_NEG: return eval_binop_arith(rt, LV_BIN_SUB, vtx_make_smi(0), v);
        case LV_UN_NOT: return vtx_make_bool(!lv_is_truthy(v));
        case LV_UN_LEN: {
            vtx_value_t argv[1] = {v};
            return lv_stdlib_dispatch(rt, 227, argv, 1);
        }
        case LV_UN_BNOT: return eval_binop_arith(rt, LV_BIN_BXOR, v, vtx_make_smi(-1));
        }
        return VTX_VALUE_NULL;
    }
    case LV_NODE_CALL: {
        int nargs = node->nchildren - 1;
        vtx_value_t *args = nargs > 0 ? lv_alloc(sizeof(vtx_value_t) * nargs) : NULL;
        for (int i = 0; i < nargs; i++) {
            args[i] = eval_expr(rt, node->children[i + 1], scope);
        }
        vtx_value_t fn_val = eval_expr(rt, node->children[0], scope);
        vtx_value_t r = lv_runtime_call(rt, fn_val, args, nargs);
        if (args) lv_free(args);
        return r;
    }
    case LV_NODE_METHOD_CALL: {
        vtx_value_t recv = eval_expr(rt, node->children[0], scope);
        int nargs = node->nchildren - 1;
        vtx_value_t method;
        if (lv_is_table(recv)) {
            method = lv_table_get((lv_table_t *)vtx_heap_ptr(recv),
                                  lv_runtime_intern_string(rt, node->str, strlen(node->str)));
        } else if (lv_is_string(recv)) {
            lv_table_t *strlib = lv_stdlib_string_lib(rt);
            method = strlib ? lv_table_get(strlib,
                      lv_runtime_intern_string(rt, node->str, strlen(node->str)))
                            : VTX_VALUE_NULL;
        } else {
            lv_error(rt, "attempt to index a %s value", lv_type_name(recv));
            return VTX_VALUE_NULL;
        }
        if (!lv_is_function(method)) {
            lv_error(rt, "attempt to call method '%s' (not a function)", node->str);
            return VTX_VALUE_NULL;
        }
        int total = 1 + nargs;
        vtx_value_t *args = lv_alloc(sizeof(vtx_value_t) * total);
        args[0] = recv;
        for (int i = 0; i < nargs; i++) {
            args[i + 1] = eval_expr(rt, node->children[i + 1], scope);
        }
        vtx_value_t r = lv_runtime_call(rt, method, args, total);
        lv_free(args);
        return r;
    }
    case LV_NODE_FUNCTION: {
        int proto_id = lv_runtime_register_proto(rt, node->proto);
        vtx_value_t v = lv_runtime_create_closure(rt, proto_id);
        /* Capture the enclosing scope so the function can access
         * locals from its definition site (for recursive local
         * functions and closures). */
        if (lv_is_function(v)) {
            lv_function_t *f = (lv_function_t *)vtx_heap_ptr(v);
            f->enclosing_scope = scope;
            f->rt = rt;
        }
        return v;
    }
    default:
        lv_error(rt, "internal: unhandled expr kind %d", node->kind);
        return VTX_VALUE_NULL;
    }
}

/* ---- Statement evaluation ---- */

static void eval_local(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    int nvals = node->nchildren;
    /* Special case: `local function f(...) ... end` — declare f as nil
     * first, then evaluate the function (so it can see itself for
     * recursion), then assign. */
    if (nvals == 1 && node->nnames == 1 && node->children[0]->kind == LV_NODE_FUNCTION) {
        lv_eval_scope_declare(scope, node->names[0], VTX_VALUE_NULL);
        vtx_value_t v = eval_expr(rt, node->children[0], scope);
        lv_eval_scope_set(scope, node->names[0], v);
        return;
    }
    for (int i = 0; i < node->nnames; i++) {
        vtx_value_t v = (i < nvals) ? eval_expr(rt, node->children[i], scope) : VTX_VALUE_NULL;
        lv_eval_scope_declare(scope, node->names[i], v);
    }
}

static void eval_assign(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    int ntargets = node->nchildren / 2;
    int nvals    = node->nchildren - ntargets;
    for (int i = 0; i < ntargets; i++) {
        lv_node_t *target = node->children[i];
        lv_node_t *val    = (i < nvals) ? node->children[ntargets + i] : NULL;
        vtx_value_t v = val ? eval_expr(rt, val, scope) : VTX_VALUE_NULL;
        if (target->kind == LV_NODE_NAME) {
            lv_eval_scope_set(scope, target->str, v);
        } else if (target->kind == LV_NODE_FIELD) {
            vtx_value_t t = eval_expr(rt, target->children[0], scope);
            if (!lv_is_table(t)) {
                lv_error(rt, "attempt to index a %s value", lv_type_name(t));
                return;
            }
            lv_table_set((lv_table_t *)vtx_heap_ptr(t),
                         lv_runtime_intern_string(rt, target->str, strlen(target->str)),
                         v);
        } else if (target->kind == LV_NODE_INDEX) {
            vtx_value_t t = eval_expr(rt, target->children[0], scope);
            vtx_value_t k = eval_expr(rt, target->children[1], scope);
            if (!lv_is_table(t)) {
                lv_error(rt, "attempt to index a %s value", lv_type_name(t));
                return;
            }
            lv_table_set((lv_table_t *)vtx_heap_ptr(t), k, v);
        }
    }
}

static void eval_call_stmt(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    eval_expr(rt, node->children[0], scope);
}

static void eval_do(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *parent) {
    lv_eval_scope_t *scope = lv_eval_scope_new(parent, parent->env);
    eval_block(rt, node, scope);
    lv_eval_scope_free(scope);
}

static void eval_while(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    for (;;) {
        vtx_value_t cond = eval_expr(rt, node->children[0], scope);
        if (!lv_is_truthy(cond)) break;
        lv_eval_scope_t *body_scope = lv_eval_scope_new(scope, scope->env);
        lv_sig_buf_t sigbuf;
        lv_sig_buf_t *prev = g_cur_sig;
        g_cur_sig = &sigbuf;
        if (setjmp(sigbuf.jump) == 0) {
            for (int i = 1; i < node->nchildren; i++) {
                eval_stmt(rt, node->children[i], body_scope);
            }
        } else if (sigbuf.kind == LV_SIG_BREAK) {
            g_cur_sig = prev;
            lv_eval_scope_free(body_scope);
            return;
        }
        g_cur_sig = prev;
        lv_eval_scope_free(body_scope);
    }
}

static void eval_repeat(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *parent) {
    int n = node->nchildren;
    for (;;) {
        lv_eval_scope_t *scope = lv_eval_scope_new(parent, parent->env);
        lv_sig_buf_t sigbuf;
        lv_sig_buf_t *prev = g_cur_sig;
        g_cur_sig = &sigbuf;
        bool broke = false;
        if (setjmp(sigbuf.jump) == 0) {
            for (int i = 0; i < n - 1; i++) {
                eval_stmt(rt, node->children[i], scope);
            }
        } else if (sigbuf.kind == LV_SIG_BREAK) {
            broke = true;
        }
        g_cur_sig = prev;
        if (broke) {
            lv_eval_scope_free(scope);
            return;
        }
        vtx_value_t cond = eval_expr(rt, node->children[n - 1], scope);
        lv_eval_scope_free(scope);
        if (lv_is_truthy(cond)) break;
    }
}

static void eval_if(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    int n = node->nchildren;
    int i = 0;
    while (i < n) {
        lv_node_t *cond = node->children[i];
        lv_node_t *body = (i + 1 < n) ? node->children[i + 1] : NULL;
        if (!cond) break;
        vtx_value_t cv = eval_expr(rt, cond, scope);
        if (lv_is_truthy(cv)) {
            if (body) {
                lv_eval_scope_t *body_scope = lv_eval_scope_new(scope, scope->env);
                if (body->kind == LV_NODE_DO) {
                    eval_block(rt, body, body_scope);
                } else {
                    eval_stmt(rt, body, body_scope);
                }
                lv_eval_scope_free(body_scope);
            }
            return;
        }
        i += 2;
        if (i == n - 1) {
            /* else body */
            lv_node_t *eb = node->children[i];
            lv_eval_scope_t *body_scope = lv_eval_scope_new(scope, scope->env);
            if (eb->kind == LV_NODE_DO) {
                eval_block(rt, eb, body_scope);
            } else {
                eval_stmt(rt, eb, body_scope);
            }
            lv_eval_scope_free(body_scope);
            return;
        }
    }
}

static void eval_for_num(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *parent) {
    int64_t init, limit, step = 1;
    if (!lv_to_int(eval_expr(rt, node->exprs[0], parent), &init) ||
        !lv_to_int(eval_expr(rt, node->exprs[1], parent), &limit)) {
        lv_error(rt, "for loop: invalid initial/limit value");
        return;
    }
    if (node->exprs[2]) {
        if (!lv_to_int(eval_expr(rt, node->exprs[2], parent), &step)) {
            lv_error(rt, "for loop: invalid step value");
            return;
        }
    }
    if (step == 0) { lv_error(rt, "for loop: step cannot be zero"); return; }

    lv_sig_buf_t sigbuf;
    lv_sig_buf_t *prev = g_cur_sig;
    g_cur_sig = &sigbuf;
    for (int64_t v = init; (step > 0 ? v <= limit : v >= limit); v += step) {
        lv_eval_scope_t *scope = lv_eval_scope_new(parent, parent->env);
        lv_eval_scope_declare(scope, node->names[0], vtx_make_smi(v));
        bool broke = false;
        if (setjmp(sigbuf.jump) == 0) {
            for (int i = 0; i < node->nchildren; i++) {
                eval_stmt(rt, node->children[i], scope);
            }
        } else if (sigbuf.kind == LV_SIG_BREAK) {
            broke = true;
        }
        lv_eval_scope_free(scope);
        if (broke) break;
    }
    g_cur_sig = prev;
}

static void eval_for_in(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *parent) {
    /* For MVP: detect pairs/ipairs and iterate directly. */
    int nexprs = 0;
    while (nexprs < node->nchildren &&
           (node->children[nexprs]->kind == LV_NODE_CALL ||
            node->children[nexprs]->kind == LV_NODE_METHOD_CALL ||
            node->children[nexprs]->kind == LV_NODE_NAME ||
            node->children[nexprs]->kind == LV_NODE_FIELD ||
            node->children[nexprs]->kind == LV_NODE_INDEX)) {
        nexprs++;
    }
    int nbody = node->nchildren - nexprs;

    if (nexprs == 1 && node->children[0]->kind == LV_NODE_CALL) {
        lv_node_t *call = node->children[0];
        lv_node_t *callee = call->children[0];
        if (callee->kind == LV_NODE_NAME &&
            (strcmp(callee->str, "pairs") == 0 || strcmp(callee->str, "ipairs") == 0) &&
            call->nchildren >= 2) {
            bool is_pairs = (callee->str[0] == 'p');
            vtx_value_t tv = eval_expr(rt, call->children[1], parent);
            if (!lv_is_table(tv)) {
                lv_error(rt, "for-in: argument must be a table");
                return;
            }
            lv_table_t *t = (lv_table_t *)vtx_heap_ptr(tv);
            lv_sig_buf_t sigbuf;
            lv_sig_buf_t *prev = g_cur_sig;
            g_cur_sig = &sigbuf;
            if (is_pairs) {
                vtx_value_t key = VTX_VALUE_UNDEFINED;
                for (;;) {
                    key = lv_table_next(t, key);
                    if (lv_is_nil(key)) break;
                    vtx_value_t val = lv_table_get(t, key);
                    lv_eval_scope_t *scope = lv_eval_scope_new(parent, parent->env);
                    if (node->nnames >= 1) lv_eval_scope_declare(scope, node->names[0], key);
                    if (node->nnames >= 2) lv_eval_scope_declare(scope, node->names[1], val);
                    bool broke = false;
                    if (setjmp(sigbuf.jump) == 0) {
                        for (int i = 0; i < nbody; i++) {
                            eval_stmt(rt, node->children[nexprs + i], scope);
                        }
                    } else if (sigbuf.kind == LV_SIG_BREAK) {
                        broke = true;
                    }
                    lv_eval_scope_free(scope);
                    if (broke) break;
                }
            } else {
                /* ipairs: integer keys 1, 2, ... */
                for (int64_t i = 1; ; i++) {
                    vtx_value_t val = lv_table_get(t, vtx_make_smi(i));
                    if (lv_is_nil(val)) break;
                    lv_eval_scope_t *scope = lv_eval_scope_new(parent, parent->env);
                    if (node->nnames >= 1) lv_eval_scope_declare(scope, node->names[0], vtx_make_smi(i));
                    if (node->nnames >= 2) lv_eval_scope_declare(scope, node->names[1], val);
                    bool broke = false;
                    if (setjmp(sigbuf.jump) == 0) {
                        for (int j = 0; j < nbody; j++) {
                            eval_stmt(rt, node->children[nexprs + j], scope);
                        }
                    } else if (sigbuf.kind == LV_SIG_BREAK) {
                        broke = true;
                    }
                    lv_eval_scope_free(scope);
                    if (broke) break;
                }
            }
            g_cur_sig = prev;
            return;
        }
    }
    lv_error(rt, "for-in: only pairs(t)/ipairs(t) are supported in MVP");
}

static void eval_return(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    vtx_value_t v = VTX_VALUE_NULL;
    if (node->nchildren > 0) {
        v = eval_expr(rt, node->children[0], scope);
    }
    if (g_cur_sig) {
        g_cur_sig->kind  = LV_SIG_RETURN;
        g_cur_sig->value = v;
        longjmp(g_cur_sig->jump, 1);
    }
    /* No active signal — this is a top-level return; just ignore. */
}

static void eval_break(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    (void)rt; (void)node; (void)scope;
    if (!g_cur_sig) {
        lv_error(NULL, "'break' outside of a loop");
        return;
    }
    g_cur_sig->kind = LV_SIG_BREAK;
    g_cur_sig->value = VTX_VALUE_NULL;
    longjmp(g_cur_sig->jump, 1);
}

static void eval_goto(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    (void)rt; (void)node; (void)scope;
    /* goto not supported in MVP eval */
}

static void eval_label(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    (void)rt; (void)node; (void)scope;
}

static void eval_stmt(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope) {
    if (!node) return;
    switch (node->kind) {
    case LV_NODE_LOCAL:     eval_local(rt, node, scope); return;
    case LV_NODE_ASSIGN:    eval_assign(rt, node, scope); return;
    case LV_NODE_CALL_STMT: eval_call_stmt(rt, node, scope); return;
    case LV_NODE_DO:        eval_do(rt, node, scope); return;
    case LV_NODE_WHILE:     eval_while(rt, node, scope); return;
    case LV_NODE_REPEAT:    eval_repeat(rt, node, scope); return;
    case LV_NODE_IF:        eval_if(rt, node, scope); return;
    case LV_NODE_FOR_NUM:   eval_for_num(rt, node, scope); return;
    case LV_NODE_FOR_IN:    eval_for_in(rt, node, scope); return;
    case LV_NODE_RETURN:    eval_return(rt, node, scope); return;
    case LV_NODE_BREAK:     eval_break(rt, node, scope); return;
    case LV_NODE_GOTO:      eval_goto(rt, node, scope); return;
    case LV_NODE_LABEL:     eval_label(rt, node, scope); return;
    default:
        /* expression statement: evaluate and discard */
        eval_expr(rt, node, scope);
        return;
    }
}

static void eval_block(lv_runtime_t *rt, lv_node_t *block, lv_eval_scope_t *scope) {
    if (!block) return;
    if (block->kind == LV_NODE_DO || block->kind == LV_NODE_CHUNK) {
        for (int i = 0; i < block->nchildren; i++) {
            eval_stmt(rt, block->children[i], scope);
        }
    }
}

/* Setters for the thread-local runtime pointer. */
void lv_eval_set_runtime(lv_runtime_t *rt) { g_cur_rt = rt; }
lv_runtime_t *lv_eval_get_runtime(void) { return g_cur_rt; }

/* ---- Top-level entry: evaluate a function proto ---- */

/* Evaluate a function proto with an explicit enclosing scope (for
 * closures that captured locals from their definition site). */
vtx_value_t lv_runtime_eval_proto_scope(lv_runtime_t *rt, lv_func_proto_t *proto,
                                         vtx_value_t *args, int nargs,
                                         lv_eval_scope_t *enclosing) {
    lv_eval_set_runtime(rt);
    lv_eval_scope_t *scope = lv_eval_scope_new(enclosing, rt->current_env);

    /* Bind parameters. */
    for (int i = 0; i < proto->nparams; i++) {
        vtx_value_t v = (i < nargs) ? args[i] : VTX_VALUE_NULL;
        lv_eval_scope_declare(scope, proto->params[i], v);
    }

    /* Set up return signal. */
    lv_sig_buf_t sigbuf;
    sigbuf.kind = LV_SIG_NORMAL;
    sigbuf.value = VTX_VALUE_NULL;
    lv_sig_buf_t *prev = g_cur_sig;
    g_cur_sig = &sigbuf;

    vtx_value_t result = VTX_VALUE_NULL;
    if (setjmp(sigbuf.jump) == 0) {
        /* Execute body. */
        for (int i = 0; i < proto->nbody; i++) {
            eval_stmt(rt, proto->body[i], scope);
        }
    } else if (sigbuf.kind == LV_SIG_RETURN) {
        result = sigbuf.value;
    }
    g_cur_sig = prev;
    lv_eval_scope_free(scope);
    return result;
}

/* Evaluate a function proto with no enclosing scope (top-level call). */
vtx_value_t lv_runtime_eval_proto(lv_runtime_t *rt, lv_func_proto_t *proto,
                                   vtx_value_t *args, int nargs) {
    return lv_runtime_eval_proto_scope(rt, proto, args, nargs, NULL);
}
