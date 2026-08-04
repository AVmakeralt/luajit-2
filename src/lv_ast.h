/* lv_ast.h — Abstract syntax tree node types for LuaVortex.
 *
 * The AST is a high-level representation produced by the parser and
 * consumed by the code generator. Each node owns its children; calling
 * lv_node_free() on the root recursively frees the entire tree.
 */

#ifndef LV_AST_H
#define LV_AST_H

#include "lv.h"
#include "lv_lexer.h" /* for lv_token_kind_t */

/* ---- Forward declarations ---- */
typedef struct lv_node       lv_node_t;
typedef struct lv_func_proto lv_func_proto_t;

/* ---- Node kinds ---- */
typedef enum {
    /* Literals */
    LV_NODE_NIL,
    LV_NODE_TRUE,
    LV_NODE_FALSE,
    LV_NODE_INT,        /* value in n.ival */
    LV_NODE_FLOAT,      /* value in n.fval */
    LV_NODE_STRING,     /* value in n.str / n.str_len */
    LV_NODE_DOTS,       /* ... */

    /* Variables */
    LV_NODE_NAME,       /* identifier; name in n.str */
    LV_NODE_INDEX,      /* t[k]    : children[0]=t, children[1]=k */
    LV_NODE_FIELD,      /* t.name  : children[0]=t, name in n.str */

    /* Table constructor */
    LV_NODE_TABLE,      /* { ... } : children are entries (positional or kv) */
    /* table entry kinds: */
    /*   LV_NODE_INT/STRING/FLOAT as key + child as value (kv)
         LV_NODE_NAME as positional (treated as int key by index) */

    /* Operations */
    LV_NODE_BINOP,      /* n.op = operator, children[0]=lhs, children[1]=rhs */
    LV_NODE_UNOP,       /* n.op = operator, children[0]=operand */
    LV_NODE_CALL,       /* children[0]=callee, children[1..]=args; n.nargs */
    LV_NODE_METHOD_CALL,/* children[0]=receiver, name in n.str, then args */

    /* Function literal */
    LV_NODE_FUNCTION,   /* n.proto = function prototype */

    /* ---- Statements ---- */
    LV_NODE_LOCAL,      /* local x1, x2 = e1, e2 : names[], children[] values */
    LV_NODE_ASSIGN,     /* targets[] = values[] : targets and values in children (contiguous) */
    LV_NODE_CALL_STMT,  /* expression statement that is a call */
    LV_NODE_DO,         /* do ... end : children[] body */
    LV_NODE_WHILE,      /* children[0]=cond, children[1..]=body */
    LV_NODE_REPEAT,     /* children[0..n-1]=body, children[n]=cond */
    LV_NODE_IF,         /* children[0]=cond, children[1..m]=then-body,
                           children[m+1]=else-cond (for elseif) or final else-body */
    LV_NODE_FOR_NUM,    /* numeric for: names[0]=var, names[1]=limit, names[2]=step;
                           children[0..]=body. Expressions for init/limit/step are
                           stored in exprs[0..2]. */
    LV_NODE_FOR_IN,     /* generic for: names[] = exprs[]; children[] body */
    LV_NODE_RETURN,     /* children[] = return values */
    LV_NODE_BREAK,
    LV_NODE_GOTO,       /* name in n.str */
    LV_NODE_LABEL,      /* name in n.str */

    /* Chunk (root) */
    LV_NODE_CHUNK,      /* children[] are statements */
} lv_node_kind_t;

/* ---- Binary operators ---- */
typedef enum {
    LV_BIN_ADD, LV_BIN_SUB, LV_BIN_MUL, LV_BIN_DIV, LV_BIN_IDIV,
    LV_BIN_MOD, LV_BIN_POW,
    LV_BIN_CONCAT,
    LV_BIN_EQ, LV_BIN_NE, LV_BIN_LT, LV_BIN_LE, LV_BIN_GT, LV_BIN_GE,
    LV_BIN_AND, LV_BIN_OR,
    LV_BIN_BAND, LV_BIN_BOR, LV_BIN_BXOR, LV_BIN_SHL, LV_BIN_SHR,
} lv_binop_t;

/* ---- Unary operators ---- */
typedef enum {
    LV_UN_NEG, LV_UN_NOT, LV_UN_LEN, LV_UN_BNOT,
} lv_unop_t;

/* ---- Function prototype ----
 * A function prototype captures everything the codegen needs to emit
 * a function: parameter names, body, and (resolved later) upvalue
 * information. */
struct lv_func_proto {
    char  *name;            /* function name (may be NULL for anonymous) */
    char **params;          /* parameter names (NULL-terminated array) */
    int    nparams;
    bool   is_vararg;       /* true if declared with ... */
    lv_node_t **body;       /* body statements (NULL-terminated array) */
    int    nbody;
    int    line;
};

/* ---- Node ---- */
struct lv_node {
    lv_node_kind_t kind;
    int line;

    /* Literal values */
    int64_t ival;
    double  fval;
    char    *str;          /* for LV_NODE_STRING, LV_NODE_NAME, LV_NODE_GOTO, etc. */
    size_t  str_len;

    /* Operator (LV_NODE_BINOP / LV_NODE_UNOP) */
    lv_binop_t binop;
    lv_unop_t  unop;

    /* Names list (LV_NODE_LOCAL: declared names; LV_NODE_FOR_IN: loop vars) */
    char **names;
    int    nnames;

    /* Function prototype (LV_NODE_FUNCTION) */
    lv_func_proto_t *proto;

    /* Children (NULL-terminated array of nodes) */
    lv_node_t **children;
    int    nchildren;

    /* Auxiliary expressions for numeric for */
    lv_node_t *exprs[3];
};

/* ---- Constructors (each allocates and returns a new node) ---- */
lv_node_t *lv_node_new_nil(int line);
lv_node_t *lv_node_new_true(int line);
lv_node_t *lv_node_new_false(int line);
lv_node_t *lv_node_new_int(int line, int64_t v);
lv_node_t *lv_node_new_float(int line, double v);
lv_node_t *lv_node_new_string(int line, const char *s, size_t len);
lv_node_t *lv_node_new_dots(int line);
lv_node_t *lv_node_new_name(int line, const char *name);
lv_node_t *lv_node_new_index(int line, lv_node_t *t, lv_node_t *k);
lv_node_t *lv_node_new_field(int line, lv_node_t *t, const char *name);
lv_node_t *lv_node_new_table(int line);
lv_node_t *lv_node_new_binop(int line, lv_binop_t op, lv_node_t *l, lv_node_t *r);
lv_node_t *lv_node_new_unop(int line, lv_unop_t op, lv_node_t *operand);
lv_node_t *lv_node_new_call(int line, lv_node_t *fn, lv_node_t **args, int nargs);
lv_node_t *lv_node_new_method_call(int line, lv_node_t *recv, const char *name,
                                    lv_node_t **args, int nargs);
lv_node_t *lv_node_new_function(int line, lv_func_proto_t *proto);
lv_node_t *lv_node_new_local(int line, char **names, int nnames, lv_node_t **vals, int nvals);
lv_node_t *lv_node_new_assign(int line, lv_node_t **targets, int ntargets,
                               lv_node_t **vals, int nvals);
lv_node_t *lv_node_new_call_stmt(int line, lv_node_t *call);
lv_node_t *lv_node_new_do(int line, lv_node_t **body, int nbody);
lv_node_t *lv_node_new_while(int line, lv_node_t *cond, lv_node_t **body, int nbody);
lv_node_t *lv_node_new_repeat(int line, lv_node_t **body, int nbody, lv_node_t *cond);
lv_node_t *lv_node_new_if(int line);
lv_node_t *lv_node_new_for_num(int line, const char *var_name,
                                lv_node_t *init, lv_node_t *limit, lv_node_t *step,
                                lv_node_t **body, int nbody);
lv_node_t *lv_node_new_for_in(int line, char **names, int nnames,
                               lv_node_t **exprs, int nexprs,
                               lv_node_t **body, int nbody);
lv_node_t *lv_node_new_return(int line, lv_node_t **vals, int nvals);
lv_node_t *lv_node_new_break(int line);
lv_node_t *lv_node_new_goto(int line, const char *label);
lv_node_t *lv_node_new_label(int line, const char *label);
lv_node_t *lv_node_new_chunk(int line, lv_node_t **body, int nbody);

/* Append a child to a node (for building tables / if-chains). */
void lv_node_append_child(lv_node_t *node, lv_node_t *child);

/* Recursively free a node and all its children. */
void lv_node_free(lv_node_t *node);

/* Free a function prototype. */
void lv_func_proto_free(lv_func_proto_t *proto);

/* Human-readable name for a binary operator. */
const char *lv_binop_name(lv_binop_t op);
const char *lv_unop_name(lv_unop_t op);

#endif /* LV_AST_H */
