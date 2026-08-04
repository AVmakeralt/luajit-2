/* lv_ast.c — AST node constructors. */

#include "lv_ast.h"

/* Allocate a zeroed node of the given kind. */
static lv_node_t *node_alloc(lv_node_kind_t k, int line) {
    lv_node_t *n = lv_alloc(sizeof(lv_node_t));
    memset(n, 0, sizeof(*n));
    n->kind = k;
    n->line = line;
    return n;
}

/* Duplicate a NULL-terminated string array of length cnt. */
static char **dup_strings(char **src, int cnt) {
    if (cnt <= 0) return NULL;
    char **dst = lv_alloc(sizeof(char *) * cnt);
    for (int i = 0; i < cnt; i++) dst[i] = lv_strdup(src[i]);
    return dst;
}

/* Copy a node array (the array is copied; nodes themselves are shared
 * by reference — ownership of the nodes transfers to the new array). */
static lv_node_t **dup_nodes(lv_node_t **src, int cnt) {
    if (cnt <= 0) return NULL;
    lv_node_t **dst = lv_alloc(sizeof(lv_node_t *) * cnt);
    memcpy(dst, src, sizeof(lv_node_t *) * cnt);
    return dst;
}

lv_node_t *lv_node_new_nil(int line)         { return node_alloc(LV_NODE_NIL, line); }
lv_node_t *lv_node_new_true(int line)        { return node_alloc(LV_NODE_TRUE, line); }
lv_node_t *lv_node_new_false(int line)       { return node_alloc(LV_NODE_FALSE, line); }

lv_node_t *lv_node_new_int(int line, int64_t v) {
    lv_node_t *n = node_alloc(LV_NODE_INT, line);
    n->ival = v;
    return n;
}

lv_node_t *lv_node_new_float(int line, double v) {
    lv_node_t *n = node_alloc(LV_NODE_FLOAT, line);
    n->fval = v;
    return n;
}

lv_node_t *lv_node_new_string(int line, const char *s, size_t len) {
    lv_node_t *n = node_alloc(LV_NODE_STRING, line);
    n->str     = lv_strndup(s, len);
    n->str_len = len;
    return n;
}

lv_node_t *lv_node_new_dots(int line) { return node_alloc(LV_NODE_DOTS, line); }

lv_node_t *lv_node_new_name(int line, const char *name) {
    lv_node_t *n = node_alloc(LV_NODE_NAME, line);
    n->str = lv_strdup(name);
    return n;
}

lv_node_t *lv_node_new_index(int line, lv_node_t *t, lv_node_t *k) {
    lv_node_t *n = node_alloc(LV_NODE_INDEX, line);
    n->children = lv_alloc(sizeof(lv_node_t *) * 2);
    n->children[0] = t;
    n->children[1] = k;
    n->nchildren = 2;
    return n;
}

lv_node_t *lv_node_new_field(int line, lv_node_t *t, const char *name) {
    lv_node_t *n = node_alloc(LV_NODE_FIELD, line);
    n->str = lv_strdup(name);
    n->children = lv_alloc(sizeof(lv_node_t *) * 1);
    n->children[0] = t;
    n->nchildren = 1;
    return n;
}

lv_node_t *lv_node_new_table(int line) {
    lv_node_t *n = node_alloc(LV_NODE_TABLE, line);
    return n;
}

lv_node_t *lv_node_new_binop(int line, lv_binop_t op, lv_node_t *l, lv_node_t *r) {
    lv_node_t *n = node_alloc(LV_NODE_BINOP, line);
    n->binop = op;
    n->children = lv_alloc(sizeof(lv_node_t *) * 2);
    n->children[0] = l;
    n->children[1] = r;
    n->nchildren = 2;
    return n;
}

lv_node_t *lv_node_new_unop(int line, lv_unop_t op, lv_node_t *operand) {
    lv_node_t *n = node_alloc(LV_NODE_UNOP, line);
    n->unop = op;
    n->children = lv_alloc(sizeof(lv_node_t *) * 1);
    n->children[0] = operand;
    n->nchildren = 1;
    return n;
}

lv_node_t *lv_node_new_call(int line, lv_node_t *fn, lv_node_t **args, int nargs) {
    lv_node_t *n = node_alloc(LV_NODE_CALL, line);
    n->children = lv_alloc(sizeof(lv_node_t *) * (nargs + 1));
    n->children[0] = fn;
    for (int i = 0; i < nargs; i++) n->children[i + 1] = args[i];
    n->nchildren = nargs + 1;
    return n;
}

lv_node_t *lv_node_new_method_call(int line, lv_node_t *recv, const char *name,
                                    lv_node_t **args, int nargs) {
    lv_node_t *n = node_alloc(LV_NODE_METHOD_CALL, line);
    n->str = lv_strdup(name);
    n->children = lv_alloc(sizeof(lv_node_t *) * (nargs + 1));
    n->children[0] = recv;
    for (int i = 0; i < nargs; i++) n->children[i + 1] = args[i];
    n->nchildren = nargs + 1;
    return n;
}

lv_node_t *lv_node_new_function(int line, lv_func_proto_t *proto) {
    lv_node_t *n = node_alloc(LV_NODE_FUNCTION, line);
    n->proto = proto;
    return n;
}

lv_node_t *lv_node_new_local(int line, char **names, int nnames, lv_node_t **vals, int nvals) {
    lv_node_t *n = node_alloc(LV_NODE_LOCAL, line);
    n->names  = dup_strings(names, nnames);
    n->nnames = nnames;
    n->children = dup_nodes(vals, nvals);
    n->nchildren = nvals;
    return n;
}

lv_node_t *lv_node_new_assign(int line, lv_node_t **targets, int ntargets,
                               lv_node_t **vals, int nvals) {
    lv_node_t *n = node_alloc(LV_NODE_ASSIGN, line);
    /* Store targets then values contiguously; nchildren = ntargets + nvals */
    n->children = lv_alloc(sizeof(lv_node_t *) * (ntargets + nvals));
    for (int i = 0; i < ntargets; i++) n->children[i] = targets[i];
    for (int i = 0; i < nvals;    i++) n->children[ntargets + i] = vals[i];
    n->nchildren = ntargets + nvals;
    return n;
}

lv_node_t *lv_node_new_call_stmt(int line, lv_node_t *call) {
    lv_node_t *n = node_alloc(LV_NODE_CALL_STMT, line);
    n->children = lv_alloc(sizeof(lv_node_t *) * 1);
    n->children[0] = call;
    n->nchildren = 1;
    return n;
}

lv_node_t *lv_node_new_do(int line, lv_node_t **body, int nbody) {
    lv_node_t *n = node_alloc(LV_NODE_DO, line);
    n->children = dup_nodes(body, nbody);
    n->nchildren = nbody;
    return n;
}

lv_node_t *lv_node_new_while(int line, lv_node_t *cond, lv_node_t **body, int nbody) {
    lv_node_t *n = node_alloc(LV_NODE_WHILE, line);
    n->children = lv_alloc(sizeof(lv_node_t *) * (nbody + 1));
    n->children[0] = cond;
    for (int i = 0; i < nbody; i++) n->children[i + 1] = body[i];
    n->nchildren = nbody + 1;
    return n;
}

lv_node_t *lv_node_new_repeat(int line, lv_node_t **body, int nbody, lv_node_t *cond) {
    lv_node_t *n = node_alloc(LV_NODE_REPEAT, line);
    n->children = lv_alloc(sizeof(lv_node_t *) * (nbody + 1));
    for (int i = 0; i < nbody; i++) n->children[i] = body[i];
    n->children[nbody] = cond;
    n->nchildren = nbody + 1;
    return n;
}

lv_node_t *lv_node_new_if(int line) {
    return node_alloc(LV_NODE_IF, line);
}

lv_node_t *lv_node_new_for_num(int line, const char *var_name,
                                lv_node_t *init, lv_node_t *limit, lv_node_t *step,
                                lv_node_t **body, int nbody) {
    lv_node_t *n = node_alloc(LV_NODE_FOR_NUM, line);
    n->names = lv_alloc(sizeof(char *) * 1);
    n->names[0] = lv_strdup(var_name);
    n->nnames = 1;
    n->exprs[0] = init;
    n->exprs[1] = limit;
    n->exprs[2] = step;  /* may be NULL */
    n->children = dup_nodes(body, nbody);
    n->nchildren = nbody;
    return n;
}

lv_node_t *lv_node_new_for_in(int line, char **names, int nnames,
                               lv_node_t **exprs, int nexprs,
                               lv_node_t **body, int nbody) {
    lv_node_t *n = node_alloc(LV_NODE_FOR_IN, line);
    n->names  = dup_strings(names, nnames);
    n->nnames = nnames;
    /* exprs stored in children[0..nexprs-1], body in children[nexprs..] */
    n->children = lv_alloc(sizeof(lv_node_t *) * (nexprs + nbody));
    for (int i = 0; i < nexprs; i++) n->children[i] = exprs[i];
    for (int i = 0; i < nbody;  i++) n->children[nexprs + i] = body[i];
    n->nchildren = nexprs + nbody;
    return n;
}

lv_node_t *lv_node_new_return(int line, lv_node_t **vals, int nvals) {
    lv_node_t *n = node_alloc(LV_NODE_RETURN, line);
    n->children = dup_nodes(vals, nvals);
    n->nchildren = nvals;
    return n;
}

lv_node_t *lv_node_new_break(int line)   { return node_alloc(LV_NODE_BREAK, line); }

lv_node_t *lv_node_new_goto(int line, const char *label) {
    lv_node_t *n = node_alloc(LV_NODE_GOTO, line);
    n->str = lv_strdup(label);
    return n;
}

lv_node_t *lv_node_new_label(int line, const char *label) {
    lv_node_t *n = node_alloc(LV_NODE_LABEL, line);
    n->str = lv_strdup(label);
    return n;
}

lv_node_t *lv_node_new_chunk(int line, lv_node_t **body, int nbody) {
    lv_node_t *n = node_alloc(LV_NODE_CHUNK, line);
    n->children = dup_nodes(body, nbody);
    n->nchildren = nbody;
    return n;
}

void lv_node_append_child(lv_node_t *node, lv_node_t *child) {
    node->children = lv_realloc(node->children, sizeof(lv_node_t *) * (node->nchildren + 1));
    node->children[node->nchildren++] = child;
}

void lv_func_proto_free(lv_func_proto_t *proto) {
    if (!proto) return;
    if (proto->name)   lv_free(proto->name);
    if (proto->params) {
        for (int i = 0; i < proto->nparams; i++) lv_free(proto->params[i]);
        lv_free(proto->params);
    }
    if (proto->body) {
        for (int i = 0; i < proto->nbody; i++) lv_node_free(proto->body[i]);
        lv_free(proto->body);
    }
    lv_free(proto);
}

void lv_node_free(lv_node_t *node) {
    if (!node) return;
    if (node->str)      lv_free(node->str);
    if (node->names) {
        for (int i = 0; i < node->nnames; i++) lv_free(node->names[i]);
        lv_free(node->names);
    }
    if (node->children) {
        for (int i = 0; i < node->nchildren; i++) lv_node_free(node->children[i]);
        lv_free(node->children);
    }
    if (node->proto) lv_func_proto_free(node->proto);
    /* exprs[] (numeric for) are NOT owned by the node — they're aliased
     * in children[]. Don't free them here. */
    lv_free(node);
}

const char *lv_binop_name(lv_binop_t op) {
    switch (op) {
    case LV_BIN_ADD:   return "+";
    case LV_BIN_SUB:   return "-";
    case LV_BIN_MUL:   return "*";
    case LV_BIN_DIV:   return "/";
    case LV_BIN_IDIV:  return "//";
    case LV_BIN_MOD:   return "%";
    case LV_BIN_POW:   return "^";
    case LV_BIN_CONCAT:return "..";
    case LV_BIN_EQ:    return "==";
    case LV_BIN_NE:    return "~=";
    case LV_BIN_LT:    return "<";
    case LV_BIN_LE:    return "<=";
    case LV_BIN_GT:    return ">";
    case LV_BIN_GE:    return ">=";
    case LV_BIN_AND:   return "and";
    case LV_BIN_OR:    return "or";
    case LV_BIN_BAND:  return "&";
    case LV_BIN_BOR:   return "|";
    case LV_BIN_BXOR:  return "~";
    case LV_BIN_SHL:   return "<<";
    case LV_BIN_SHR:   return ">>";
    }
    return "?";
}

const char *lv_unop_name(lv_unop_t op) {
    switch (op) {
    case LV_UN_NEG:  return "-";
    case LV_UN_NOT:  return "not";
    case LV_UN_LEN:  return "#";
    case LV_UN_BNOT: return "~";
    }
    return "?";
}
