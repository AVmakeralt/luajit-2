/* lv_stdlib.h — Lua standard library registration and dispatch. */

#ifndef LV_STDLIB_H
#define LV_STDLIB_H

#include "lv.h"
#include "lv_value.h"
#include "lv_runtime.h"

/* Register all stdlib functions into rt->globals. */
void lv_stdlib_register(lv_runtime_t *rt);

/* Dispatch a CALL_RUNTIME with the given lua_fn_id.
 * argv[0..arg_count-1] are the arguments (already extracted from the
 * VORTEX operand stack). Returns the single result value. */
vtx_value_t lv_stdlib_dispatch(lv_runtime_t *rt, uint16_t lua_fn_id,
                                vtx_value_t *argv, int arg_count);

/* Get the string library table (for t:method() dispatch on strings). */
lv_table_t *lv_stdlib_string_lib(lv_runtime_t *rt);

#endif /* LV_STDLIB_H */
