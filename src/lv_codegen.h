/* lv_codegen.h — AST → VORTEX bytecode emitter.
 *
 * Walks the AST produced by the parser and emits a VORTEX bytecode
 * module (vtx_bytecode_t) that can be executed by vtx_runtime_run().
 *
 * The compiler uses a simple stack-based bytecode emission strategy:
 *   - Each Lua expression emits code that pushes exactly one value
 *     onto the operand stack.
 *   - Each Lua statement emits code that has net zero stack effect.
 *   - Local variables are mapped to VORTEX local slots.
 *   - Lua closures are implemented as VORTEX methods with a captured
 *     environment object passed as the first parameter.
 *
 * Runtime call protocol for Lua stdlib:
 *   CALL_RUNTIME with operand = (lua_fn_id << 6) | arg_count
 *   where lua_fn_id >= 100 (low IDs 0-6 reserved for VORTEX builtins).
 *   The interpreter pops `arg_count` values and passes them to the
 *   registered C function, which returns a single value pushed back
 *   onto the stack.
 */

#ifndef LV_CODEGEN_H
#define LV_CODEGEN_H

#include "lv.h"
#include "lv_ast.h"

/* Forward decl: the runtime owns the host function table. */
typedef struct lv_runtime lv_runtime_t;

typedef struct lv_codegen lv_codegen_t;

/* Create a new codegen. The runtime is used to register host function
 * IDs during emission (e.g., print, pairs). */
lv_codegen_t *lv_codegen_create(lv_runtime_t *rt);

void lv_codegen_destroy(lv_codegen_t *cg);

/* Compile a chunk AST into a VORTEX bytecode module.
 * Returns NULL on failure (error in cg->last_error).
 * Caller owns the returned bytecode and must free it with lv_bytecode_free. */
typedef struct lv_compiled lv_compiled_t;
lv_compiled_t *lv_codegen_compile(lv_codegen_t *cg, lv_node_t *chunk);

/* Free a compiled module. */
void lv_compiled_free(lv_compiled_t *c);

/* Access the VORTEX bytecode for execution. */
const vtx_bytecode_t *lv_compiled_bytecode(const lv_compiled_t *c);

/* Get a human-readable disassembly. Caller must not free. */
const char *lv_compiled_disasm(const lv_compiled_t *c);

/* Get the last error message (NULL if no error). */
const char *lv_codegen_last_error(const lv_codegen_t *cg);

#endif /* LV_CODEGEN_H */
