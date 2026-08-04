/* lv_value.h — Lua value model on top of VORTEX's NaN-boxing.
 *
 * Lua values are represented as vtx_value_t with the following mapping:
 *
 *   Lua nil    → VORTEX null        (VTX_VALUE_NULL)
 *   Lua true   → VORTEX true        (VTX_VALUE_TRUE)
 *   Lua false  → VORTEX false       (VTX_VALUE_FALSE)
 *   Lua int    → VORTEX SMI         (when in 48-bit SMI range)
 *   Lua float  → VORTEX double      (when out of SMI range, or has fractional part)
 *   Lua string → heap pointer to LvString (via vtx_make_heap_ptr)
 *   Lua table  → heap pointer to LvTable (via vtx_make_heap_ptr)
 *   Lua func   → heap pointer to LvFunction (via vtx_make_heap_ptr)
 *
 * Lua heap objects are allocated with malloc (not VORTEX's GC) for MVP.
 * They carry a "kind" tag at the start so we can distinguish them.
 */

#ifndef LV_VALUE_H
#define LV_VALUE_H

#include "lv.h"

/* ---- Lua heap object kinds ---- */
typedef enum {
    LV_KIND_STRING    = 1,
    LV_KIND_TABLE     = 2,
    LV_KIND_FUNCTION  = 3,
    LV_KIND_USERDATA  = 4,
} lv_kind_t;

/* ---- Common header for all Lua heap objects ---- */
typedef struct lv_object {
    lv_kind_t  kind;
    uint32_t   gc_mark;   /* reserved for future GC integration */
} lv_object_t;

/* ---- Lua string ---- */
typedef struct lv_string {
    lv_object_t hdr;
    size_t      len;
    char        data[];   /* not NUL-terminated by default; callers add NUL when needed */
} lv_string_t;

/* ---- Lua table ----
 * For MVP, a simple hash table with open addressing and linear probing.
 * Supports any key type (nil keys are not allowed; nil value deletes the entry).
 * The array part is integrated: integer keys 1..n are stored in the hash. */
typedef struct lv_table_entry {
    vtx_value_t key;     /* VTX_VALUE_UNDEFINED = empty slot */
    vtx_value_t value;
} lv_table_entry_t;

typedef struct lv_table {
    lv_object_t          hdr;
    lv_table_entry_t    *entries;
    uint32_t             capacity;   /* power of 2 */
    uint32_t             count;      /* number of non-empty slots */
    struct lv_table     *metatable;  /* may be NULL */
} lv_table_t;

/* ---- Lua function ----
 * Either a Lua closure (carries a proto_id referencing a registered
 * function prototype in the runtime) or a native C function. */
typedef struct lv_function {
    lv_object_t  hdr;
    bool         is_native;
    union {
        int      proto_id;     /* for Lua closures: index into runtime proto table */
        struct {
            vtx_value_t (*fn)(int argc, vtx_value_t *argv, void *user_data);
            void        *user_data;
        } native;
    } u;
    /* Captured upvalues (for Lua closures). */
    vtx_value_t *upvalues;
    int          nupvalues;
    /* The enclosing scope (for recursive local functions and closures).
     * Used by the tree-walker (kept for backward compat; compiled
     * closures use captured_env instead). */
    struct lv_eval_scope *enclosing_scope;
    /* The runtime pointer (so the function can be called from any context). */
    struct lv_runtime *rt;
    /* Compiled closure: captured environment table (a Lua table). */
    lv_table_t *captured_env;
    /* Compiled closure: pointer to the compiled VORTEX bytecode. */
    vtx_bytecode_t *compiled_bc;
} lv_function_t;

/* ---- Constructors ---- */
lv_string_t  *lv_string_new(const char *data, size_t len);
lv_table_t   *lv_table_new(uint32_t initial_capacity);
lv_function_t*lv_function_new_lua(int proto_id);
lv_function_t*lv_function_new_native(vtx_value_t (*fn)(int, vtx_value_t *, void *),
                                      void *user_data);

/* ---- Destructors ---- */
void lv_string_free(lv_string_t *s);
void lv_table_free(lv_table_t *t);
void lv_function_free(lv_function_t *f);

/* ---- Value constructors / accessors ---- */
static inline vtx_value_t lv_make_string_val(lv_string_t *s) {
    return vtx_make_heap_ptr(s);
}
static inline vtx_value_t lv_make_table_val(lv_table_t *t) {
    return vtx_make_heap_ptr(t);
}
static inline vtx_value_t lv_make_function_val(lv_function_t *f) {
    return vtx_make_heap_ptr(f);
}

/* Extract a Lua heap object from a value (returns NULL if not a heap ptr). */
static inline lv_object_t *lv_value_object(vtx_value_t v) {
    if (!vtx_is_heap_ptr(v)) return NULL;
    return (lv_object_t *)vtx_heap_ptr(v);
}

/* Get the kind tag of a Lua heap value (or 0 if not a Lua heap object). */
static inline lv_kind_t lv_value_kind(vtx_value_t v) {
    lv_object_t *o = lv_value_object(v);
    return o ? o->kind : (lv_kind_t)0;
}

/* ---- Type predicates (Lua semantics) ---- */
bool lv_is_string(vtx_value_t v);
bool lv_is_table(vtx_value_t v);
bool lv_is_function(vtx_value_t v);
bool lv_is_nil(vtx_value_t v);
bool lv_is_bool(vtx_value_t v);
bool lv_is_int(vtx_value_t v);
bool lv_is_float(vtx_value_t v);
bool lv_is_number(vtx_value_t v);   /* int or float */

/* ---- Lua-style truthiness ----
 * false and nil are falsy; everything else is truthy. */
bool lv_is_truthy(vtx_value_t v);

/* ---- Conversions ---- */

/* Convert any value to a Lua-style string. Caller owns the result. */
char *lv_to_string(vtx_value_t v, size_t *out_len);

/* Convert any value to a number. Returns 1 on success, 0 on failure.
 * On success, *out_is_int is set and *out_ival / *out_fval is filled. */
int lv_to_number(vtx_value_t v, bool *out_is_int, int64_t *out_ival, double *out_fval);

/* Convert any value to an integer (Lua's tonumber then floor).
 * Returns 1 on success. */
int lv_to_int(vtx_value_t v, int64_t *out);

/* Convert any value to a double. Returns 1 on success. */
int lv_to_double(vtx_value_t v, double *out);

/* Return the Lua type name (e.g., "nil", "boolean", "number", "string",
 * "table", "function"). */
const char *lv_type_name(vtx_value_t v);

/* Return the Lua type as an enum for fast dispatch. */
typedef enum {
    LV_TNIL = 0,
    LV_TBOOL,
    LV_TINT,
    LV_TFLT,
    LV_TSTR,
    LV_TTBL,
    LV_TFN,
} lv_type_t;
lv_type_t lv_type_of(vtx_value_t v);

/* ---- Hashing ----
 * Used for table keys. Any value can be a key except nil and NaN. */
uint32_t lv_hash_value(vtx_value_t v);

/* ---- Equality (Lua's == without metamethods) ----
 * Numbers compare by value (int vs float compare numerically).
 * Strings compare by content. Tables/functions/booleans compare by identity. */
bool lv_raw_equal(vtx_value_t a, vtx_value_t b);

/* ---- Table operations (used by runtime) ---- */

/* Find the slot for a key (returns a pointer to the entry). */
lv_table_entry_t *lv_table_find_slot(lv_table_t *t, vtx_value_t key);

/* Set t[key] = value. If value is nil, removes the entry. */
void lv_table_set(lv_table_t *t, vtx_value_t key, vtx_value_t value);

/* Get t[key]. Returns VTX_VALUE_NULL if not present. */
vtx_value_t lv_table_get(lv_table_t *t, vtx_value_t key);

/* Get the next key after the given one (for pairs iteration). */
vtx_value_t lv_table_next(lv_table_t *t, vtx_value_t prev_key);

/* Length of the "array part" — Lua's # operator. */
int64_t lv_table_length(lv_table_t *t);

#endif /* LV_VALUE_H */
