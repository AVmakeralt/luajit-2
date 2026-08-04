/* lv_eval.h — Tree-walking evaluator for Lua function bodies.
 *
 * The main chunk of a Lua program is compiled to VORTEX bytecode and
 * runs on the VORTEX runtime. Nested function literals (closures) are
 * represented as proto IDs; when called, their bodies are evaluated
 * by this tree-walking interpreter.
 *
 * This dual-mode execution (VORTEX bytecode for the main chunk, tree-
 * walking for nested functions) is a pragmatic MVP choice. A future
 * version will compile each function to its own VORTEX method, allowing
 * the JIT to optimize hot nested functions.
 *
 * Both modes use the same Lua value model (vtx_value_t) and the same
 * stdlib, so they interoperate seamlessly.
 */

#ifndef LV_EVAL_H
#define LV_EVAL_H

#include "lv.h"
#include "lv_value.h"
#include "lv_ast.h"
#include "lv_runtime.h"

/* A scope is a chain of name→value bindings. */
typedef struct lv_eval_scope {
    struct lv_eval_scope *parent;
    char       **names;
    vtx_value_t *vals;
    int          count;
    int          capacity;
    /* The function's "environment" — usually the global table. */
    lv_table_t *env;
} lv_eval_scope_t;

/* Create a new scope (child of parent, or top-level if parent is NULL). */
lv_eval_scope_t *lv_eval_scope_new(lv_eval_scope_t *parent, lv_table_t *env);

/* Free a scope (does not free parent). */
void lv_eval_scope_free(lv_eval_scope_t *scope);

/* Look up a name in the scope chain. Returns the value or VTX_VALUE_UNDEFINED. */
vtx_value_t lv_eval_scope_get(lv_eval_scope_t *scope, const char *name);

/* Set a name in the innermost scope where it's defined; if not found,
 * set it in the global env. */
void lv_eval_scope_set(lv_eval_scope_t *scope, const char *name, vtx_value_t val);

/* Declare a new local in the given scope. */
void lv_eval_scope_declare(lv_eval_scope_t *scope, const char *name, vtx_value_t val);

/* Evaluate a function proto with the given arguments.
 * Returns the first return value (MVP: single-return).
 * Sets rt->error_msg and longjmps on error. */
vtx_value_t lv_runtime_eval_proto(lv_runtime_t *rt, lv_func_proto_t *proto,
                                   vtx_value_t *args, int nargs);

/* Evaluate a function proto with an explicit enclosing scope (for
 * closures that captured locals from their definition site). */
vtx_value_t lv_runtime_eval_proto_scope(lv_runtime_t *rt, lv_func_proto_t *proto,
                                         vtx_value_t *args, int nargs,
                                         lv_eval_scope_t *enclosing);

/* Evaluate an expression node to a value. */
vtx_value_t lv_eval_expr(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope);

/* Execute a statement node. */
void lv_eval_stmt(lv_runtime_t *rt, lv_node_t *node, lv_eval_scope_t *scope);

/* Set the thread-local "current runtime" pointer. Used by native
 * stdlib functions to access the runtime (for string interning, etc.)
 * when user_data is not available. */
void lv_eval_set_runtime(lv_runtime_t *rt);

/* Get the thread-local "current runtime" pointer (or NULL). */
lv_runtime_t *lv_eval_get_runtime(void);

#endif /* LV_EVAL_H */
