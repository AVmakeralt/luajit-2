/* lv_runtime.h — LuaVortex runtime.
 *
 * Wraps the VORTEX runtime (vtx_runtime_t) and provides:
 *   - The Lua value model (nil/bool/number/string/table/function)
 *   - The Lua standard library (print, type, tostring, ...)
 *   - The dispatch function for extended CALL_RUNTIME opcodes
 *     (lua_fn_id >= 100)
 *   - String interning (so equal string literals share storage)
 *   - Function prototype registration (for closures)
 *   - Lua-level error handling (setjmp/longjmp)
 *
 * The runtime is single-threaded (one per VORTEX runtime).
 */

#ifndef LV_RUNTIME_H
#define LV_RUNTIME_H

#include "lv.h"
#include "lv_value.h"
#include "lv_ast.h"
#include <setjmp.h>

/* ---- Lua runtime state ---- */
typedef struct lv_runtime {
    /* The underlying VORTEX runtime. */
    vtx_runtime_t   vrt;

    /* String intern table. */
    lv_string_t   **strings;
    int             nstrings;
    int             cap_strings;

    /* Function prototype table (registered by codegen). */
    lv_func_proto_t **protos;
    int              nprotos;
    int              cap_protos;

    /* The global environment. This is a Lua table that holds all
     * global variables and stdlib functions. */
    lv_table_t      *globals;

    /* The "current" environment for the running code. Usually == globals,
     * but nested closures capture their own env. */
    lv_table_t      *current_env;

    /* Lua error handling. */
    jmp_buf        *error_jmp;   /* NULL if no handler active */
    char           *error_msg;   /* owned; last error message */

    /* Varargs for the current function call (MVP: single-level). */
    vtx_value_t    *varargs;
    int             nvarargs;

    /* Return value buffer (for multi-return; MVP uses 1). */
    vtx_value_t     last_return;
} lv_runtime_t;

/* ---- Lifecycle ---- */
int  lv_runtime_create(lv_runtime_t *rt);
void lv_runtime_destroy(lv_runtime_t *rt);

/* Run a compiled module. Returns the result value. */
vtx_value_t lv_runtime_run(lv_runtime_t *rt, const vtx_bytecode_t *bc);

/* ---- String interning ----
 * Intern a string of the given length. The returned value is a heap
 * pointer to a lv_string_t. Repeated calls with the same content
 * return the same pointer. */
vtx_value_t lv_runtime_intern_string(lv_runtime_t *rt, const char *data, size_t len);

/* ---- Function prototype registration ----
 * Register a function prototype with the runtime. Returns a unique ID
 * that can be used to create closures via lv_runtime_create_closure. */
int lv_runtime_register_proto(lv_runtime_t *rt, lv_func_proto_t *proto);

/* Create a Lua closure for a registered prototype. */
vtx_value_t lv_runtime_create_closure(lv_runtime_t *rt, int proto_id);

/* Call a Lua function value with the given arguments.
 * Returns the single result value (MVP: multi-return not supported). */
vtx_value_t lv_runtime_call(lv_runtime_t *rt, vtx_value_t fn,
                             vtx_value_t *args, int nargs);

/* ---- Extended CALL_RUNTIME dispatch ----
 * Called by the patched VORTEX interpreter when it encounters a
 * CALL_RUNTIME with func_id >= 100. The opcode operand is packed as
 * (lua_fn_id << 6) | arg_count. This function pops `arg_count` values
 * from the interpreter's stack, dispatches to the Lua stdlib function
 * identified by lua_fn_id, and returns the single result value.
 *
 * The args are passed in argv[0..arg_count-1] (already extracted by
 * the interpreter patch). The result is returned as a vtx_value_t.
 */
vtx_value_t lv_runtime_dispatch(lv_runtime_t *rt, uint16_t lua_fn_id,
                                 vtx_value_t *argv, int arg_count);

/* ---- Stdlib registration ----
 * Populates rt->globals with all Lua stdlib functions (print, type,
 * tostring, tonumber, pairs, ipairs, assert, error, math.*, string.*,
 * table.*, io.*). */
void lv_stdlib_register(lv_runtime_t *rt);

/* ---- Error handling ---- */
void lv_error(lv_runtime_t *rt, const char *fmt, ...);

/* Get the current error message (NULL if no error). */
const char *lv_runtime_last_error(const lv_runtime_t *rt);

#endif /* LV_RUNTIME_H */
