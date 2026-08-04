/* lv.h — common definitions for the LuaVortex frontend.
 *
 * LuaVortex is a Lua 5.4-syntax frontend that compiles to VORTEX
 * bytecode and runs on the VORTEX multi-tier JIT runtime.
 *
 * Design:
 *   - Lexer → Parser → AST → Codegen → VORTEX bytecode
 *   - Bytecode runs on the VORTEX interpreter (T0) and is JIT-compiled
 *     by VORTEX's T1 baseline / T2 optimizing / T3 speculative tiers.
 *   - Lua values map onto VORTEX's NaN-boxed representation:
 *       Lua nil    → VORTEX null
 *       Lua bool   → VORTEX bool
 *       Lua int    → VORTEX SMI  (when in SMI range, else boxed float)
 *       Lua float  → VORTEX double
 *       Lua string → Lua heap object wrapped in vtx_make_heap_ptr
 *       Lua table  → Lua heap object wrapped in vtx_make_heap_ptr
 *       Lua func   → Lua heap object wrapped in vtx_make_heap_ptr
 *   - The Lua stdlib (print, type, tostring, string.*, table.*, ...)
 *     is implemented in C and invoked from bytecode via an extended
 *     CALL_RUNTIME protocol (see lv_runtime.h).
 */

#ifndef LV_H
#define LV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull in VORTEX runtime headers so all LuaVortex code can use
 * vtx_value_t and the NaN-boxing helpers directly. */
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/vortex_runtime.h"

/* ---- LuaVortex version ---- */
#define LV_VERSION_MAJOR 0
#define LV_VERSION_MINOR 1
#define LV_VERSION_PATCH 0
#define LV_VERSION_STRING "0.1.0"

/* ---- Error handling ----
 * LuaVortex uses a simple setjmp/longjmp-based error mechanism for
 * Lua-level errors (runtime errors, assert failures, etc.). The
 * runtime stores a jmp_buf; lv_error() longjmps to it. */

typedef struct lv_runtime lv_runtime_t;

void lv_error(lv_runtime_t *rt, const char *fmt, ...);

/* ---- Memory ----
 * For MVP, LuaVortex uses malloc/free for Lua heap objects. A future
 * version will integrate with VORTEX's generational GC. */

void *lv_alloc(size_t n);
void *lv_realloc(void *p, size_t n);
void  lv_free(void *p);

/* Duplicate a NUL-terminated string. Caller frees with lv_free. */
char *lv_strdup(const char *s);
/* Duplicate a length-prefixed string. */
char *lv_strndup(const char *s, size_t n);

#endif /* LV_H */
