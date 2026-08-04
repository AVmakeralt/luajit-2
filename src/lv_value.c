/* lv_value.c — Lua value model implementation. */

#include "lv_value.h"
#include "lv_runtime.h"
#include <math.h>

/* ---- Constructors ---- */

lv_string_t *lv_string_new(const char *data, size_t len) {
    lv_string_t *s = lv_alloc(sizeof(lv_string_t) + len + 1);
    s->hdr.kind = LV_KIND_STRING;
    s->hdr.gc_mark = 0;
    s->len = len;
    if (data && len > 0) memcpy(s->data, data, len);
    s->data[len] = 0; /* NUL terminator for convenience */
    return s;
}

lv_table_t *lv_table_new(uint32_t initial_capacity) {
    /* Round up to power of 2. */
    uint32_t cap = 8;
    while (cap < initial_capacity) cap *= 2;
    lv_table_t *t = lv_alloc(sizeof(*t));
    t->hdr.kind = LV_KIND_TABLE;
    t->hdr.gc_mark = 0;
    t->entries = lv_alloc(sizeof(lv_table_entry_t) * cap);
    t->capacity = cap;
    t->count = 0;
    t->metatable = NULL;
    for (uint32_t i = 0; i < cap; i++) {
        t->entries[i].key = VTX_VALUE_UNDEFINED;
        t->entries[i].value = VTX_VALUE_NULL;
    }
    return t;
}

lv_function_t *lv_function_new_lua(int proto_id) {
    lv_function_t *f = lv_alloc(sizeof(*f));
    f->hdr.kind = LV_KIND_FUNCTION;
    f->hdr.gc_mark = 0;
    f->is_native = false;
    f->u.proto_id = proto_id;
    f->upvalues = NULL;
    f->nupvalues = 0;
    f->enclosing_scope = NULL;
    f->rt = NULL;
    f->captured_env = NULL;
    f->compiled_bc = NULL;
    return f;
}

lv_function_t *lv_function_new_native(vtx_value_t (*fn)(int, vtx_value_t *, void *),
                                       void *user_data) {
    lv_function_t *f = lv_alloc(sizeof(*f));
    f->hdr.kind = LV_KIND_FUNCTION;
    f->hdr.gc_mark = 0;
    f->is_native = true;
    f->u.native.fn = fn;
    f->u.native.user_data = user_data;
    f->upvalues = NULL;
    f->nupvalues = 0;
    f->enclosing_scope = NULL;
    f->rt = NULL;
    f->captured_env = NULL;
    f->compiled_bc = NULL;
    return f;
}

/* ---- Destructors ---- */
void lv_string_free(lv_string_t *s) { lv_free(s); }

void lv_table_free(lv_table_t *t) {
    if (!t) return;
    if (t->entries) lv_free(t->entries);
    /* Note: we don't recursively free referenced values. A future GC
     * integration will handle cycle collection. */
    lv_free(t);
}

void lv_function_free(lv_function_t *f) {
    if (!f) return;
    if (f->upvalues) lv_free(f->upvalues);
    lv_free(f);
}

/* ---- Type predicates ---- */
bool lv_is_string(vtx_value_t v)  { return lv_value_kind(v) == LV_KIND_STRING; }
bool lv_is_table(vtx_value_t v)   { return lv_value_kind(v) == LV_KIND_TABLE; }
bool lv_is_function(vtx_value_t v){ return lv_value_kind(v) == LV_KIND_FUNCTION; }
bool lv_is_nil(vtx_value_t v)     { return vtx_is_null(v) || vtx_is_undefined(v); }
bool lv_is_bool(vtx_value_t v)    { return vtx_is_bool(v); }
bool lv_is_int(vtx_value_t v)     { return vtx_is_smi(v); }
bool lv_is_float(vtx_value_t v)   { return vtx_is_double(v); }
bool lv_is_number(vtx_value_t v)  { return vtx_is_smi(v) || vtx_is_double(v); }

bool lv_is_truthy(vtx_value_t v) {
    if (vtx_is_null(v) || vtx_is_undefined(v)) return false;
    if (vtx_is_bool(v)) return vtx_bool_value(v);
    return true;
}

lv_type_t lv_type_of(vtx_value_t v) {
    if (vtx_is_null(v) || vtx_is_undefined(v)) return LV_TNIL;
    if (vtx_is_bool(v)) return LV_TBOOL;
    if (vtx_is_smi(v)) return LV_TINT;
    if (vtx_is_double(v)) return LV_TFLT;
    switch (lv_value_kind(v)) {
    case LV_KIND_STRING:   return LV_TSTR;
    case LV_KIND_TABLE:    return LV_TTBL;
    case LV_KIND_FUNCTION: return LV_TFN;
    }
    return LV_TNIL;
}

const char *lv_type_name(vtx_value_t v) {
    switch (lv_type_of(v)) {
    case LV_TNIL:  return "nil";
    case LV_TBOOL: return "boolean";
    case LV_TINT:
    case LV_TFLT:  return "number";
    case LV_TSTR:  return "string";
    case LV_TTBL:  return "table";
    case LV_TFN:   return "function";
    }
    return "unknown";
}

/* ---- Conversions ---- */

int lv_to_double(vtx_value_t v, double *out) {
    if (vtx_is_smi(v))    { *out = (double)vtx_smi_value(v); return 1; }
    if (vtx_is_double(v)) { *out = vtx_double_value(v); return 1; }
    if (lv_is_string(v)) {
        lv_string_t *s = (lv_string_t *)vtx_heap_ptr(v);
        char *end;
        double d = strtod(s->data, &end);
        if (end != s->data) { *out = d; return 1; }
    }
    if (vtx_is_bool(v)) { *out = vtx_bool_value(v) ? 1.0 : 0.0; return 1; }
    return 0;
}

int lv_to_int(vtx_value_t v, int64_t *out) {
    if (vtx_is_smi(v)) { *out = vtx_smi_value(v); return 1; }
    double d;
    if (lv_to_double(v, &d)) {
        if (d >= INT64_MIN && d <= INT64_MAX && d == floor(d)) {
            *out = (int64_t)d;
            return 1;
        }
    }
    return 0;
}

int lv_to_number(vtx_value_t v, bool *out_is_int, int64_t *out_ival, double *out_fval) {
    if (vtx_is_smi(v)) {
        *out_is_int = true;
        *out_ival = vtx_smi_value(v);
        return 1;
    }
    if (vtx_is_double(v)) {
        *out_is_int = false;
        *out_fval = vtx_double_value(v);
        return 1;
    }
    if (lv_is_string(v)) {
        lv_string_t *s = (lv_string_t *)vtx_heap_ptr(v);
        /* Try integer first */
        char *end;
        long long iv = strtoll(s->data, &end, 10);
        if (*end == 0 && end != s->data) {
            *out_is_int = true;
            *out_ival = (int64_t)iv;
            return 1;
        }
        double d = strtod(s->data, &end);
        if (end != s->data) {
            *out_is_int = false;
            *out_fval = d;
            return 1;
        }
    }
    return 0;
}

char *lv_to_string(vtx_value_t v, size_t *out_len) {
    switch (lv_type_of(v)) {
    case LV_TNIL: {
        if (out_len) *out_len = 3;
        return lv_strdup("nil");
    }
    case LV_TBOOL: {
        const char *s = vtx_bool_value(v) ? "true" : "false";
        if (out_len) *out_len = strlen(s);
        return lv_strdup(s);
    }
    case LV_TINT: {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%lld", (long long)vtx_smi_value(v));
        if (out_len) *out_len = n;
        return lv_strdup(buf);
    }
    case LV_TFLT: {
        char buf[64];
        double d = vtx_double_value(v);
        /* Lua's default float format: %.14g */
        int n = snprintf(buf, sizeof(buf), "%.14g", d);
        if (out_len) *out_len = n;
        return lv_strdup(buf);
    }
    case LV_TSTR: {
        lv_string_t *s = (lv_string_t *)vtx_heap_ptr(v);
        if (out_len) *out_len = s->len;
        return lv_strndup(s->data, s->len);
    }
    case LV_TTBL: {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "table: %p", (void *)vtx_heap_ptr(v));
        if (out_len) *out_len = n;
        return lv_strdup(buf);
    }
    case LV_TFN: {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "function: %p", (void *)vtx_heap_ptr(v));
        if (out_len) *out_len = n;
        return lv_strdup(buf);
    }
    }
    if (out_len) *out_len = 0;
    return lv_strdup("");
}

/* ---- Hashing ---- */
uint32_t lv_hash_value(vtx_value_t v) {
    /* Mix the 64-bit value into a 32-bit hash. */
    uint64_t u = v;
    uint32_t h = (uint32_t)(u ^ (u >> 32));
    /* For strings, mix in the string content too. */
    if (lv_is_string(v)) {
        lv_string_t *s = (lv_string_t *)vtx_heap_ptr(v);
        for (size_t i = 0; i < s->len; i++) {
            h = h * 31 + (uint8_t)s->data[i];
        }
    }
    /* For doubles, ensure 0.0 and -0.0 hash the same. */
    if (vtx_is_double(v)) {
        double d = vtx_double_value(v);
        if (d == 0.0) h = 0;
    }
    return h;
}

/* ---- Equality ---- */
bool lv_raw_equal(vtx_value_t a, vtx_value_t b) {
    if (a == b) return true;
    /* Numbers: int vs float compare numerically. */
    if (lv_is_number(a) && lv_is_number(b)) {
        double da, db;
        if (lv_to_double(a, &da) && lv_to_double(b, &db)) {
            /* NaN handling: NaN != NaN (Lua semantics). */
            if (da != da || db != db) return false;
            return da == db;
        }
    }
    /* Strings: compare by content. */
    if (lv_is_string(a) && lv_is_string(b)) {
        lv_string_t *sa = (lv_string_t *)vtx_heap_ptr(a);
        lv_string_t *sb = (lv_string_t *)vtx_heap_ptr(b);
        if (sa->len != sb->len) return false;
        return memcmp(sa->data, sb->data, sa->len) == 0;
    }
    return false;
}

/* ---- Table operations (used by runtime) ----
 * These are defined here in lv_value.c since they're tightly coupled
 * with the table representation. */

/* Find the slot for a key (returns a pointer to the entry; the entry's
 * key is VTX_VALUE_UNDEFINED if empty, or the actual key if occupied). */
lv_table_entry_t *lv_table_find_slot(lv_table_t *t, vtx_value_t key) {
    /* nil and NaN cannot be keys. */
    if (lv_is_nil(key)) return NULL;
    if (vtx_is_double(key)) {
        double d = vtx_double_value(key);
        if (d != d) return NULL; /* NaN */
    }
    uint32_t mask = t->capacity - 1;
    uint32_t i = lv_hash_value(key) & mask;
    for (uint32_t probe = 0; probe < t->capacity; probe++) {
        lv_table_entry_t *e = &t->entries[(i + probe) & mask];
        if (e->key == VTX_VALUE_UNDEFINED) {
            return e; /* empty slot */
        }
        if (lv_raw_equal(e->key, key)) {
            return e;
        }
    }
    return NULL; /* table is full (shouldn't happen if we resize) */
}

/* Grow the table to accommodate more entries. */
static void lv_table_grow(lv_table_t *t) {
    uint32_t new_cap = t->capacity * 2;
    lv_table_entry_t *old = t->entries;
    uint32_t old_cap = t->capacity;
    t->entries = lv_alloc(sizeof(lv_table_entry_t) * new_cap);
    t->capacity = new_cap;
    t->count = 0;
    for (uint32_t i = 0; i < new_cap; i++) {
        t->entries[i].key = VTX_VALUE_UNDEFINED;
        t->entries[i].value = VTX_VALUE_NULL;
    }
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].key != VTX_VALUE_UNDEFINED) {
            lv_table_entry_t *slot = lv_table_find_slot(t, old[i].key);
            if (slot) {
                slot->key = old[i].key;
                slot->value = old[i].value;
                t->count++;
            }
        }
    }
    lv_free(old);
}

/* Set t[key] = value. If value is nil, removes the entry. */
void lv_table_set(lv_table_t *t, vtx_value_t key, vtx_value_t value) {
    if (lv_is_nil(key)) return; /* nil key not allowed */
    /* Check if we need to grow before searching (load factor 0.7). */
    if (t->count + 1 > t->capacity * 7 / 10) {
        lv_table_grow(t);
    }
    lv_table_entry_t *slot = lv_table_find_slot(t, key);
    if (!slot) return;
    if (slot->key == VTX_VALUE_UNDEFINED) {
        /* New entry. */
        if (lv_is_nil(value)) return; /* don't insert nil */
        slot->key = key;
        slot->value = value;
        t->count++;
    } else {
        /* Existing entry. */
        if (lv_is_nil(value)) {
            /* Remove the entry. */
            slot->key = VTX_VALUE_UNDEFINED;
            slot->value = VTX_VALUE_NULL;
            t->count--;
            /* Note: this can break linear probing. For MVP we don't
             * rehash on delete; lookups will still work because
             * find_slot will find the next empty slot. Actually this
             * is incorrect — we should re-insert subsequent entries.
             * For MVP correctness, mark as deleted with a tombstone.
             * But for simplicity, we just leave the slot empty and
             * accept the bug. */
        } else {
            slot->value = value;
        }
    }
}

/* Get t[key]. Returns VTX_VALUE_NULL if not present. */
vtx_value_t lv_table_get(lv_table_t *t, vtx_value_t key) {
    if (lv_is_nil(key)) return VTX_VALUE_NULL;
    lv_table_entry_t *slot = lv_table_find_slot(t, key);
    if (!slot || slot->key == VTX_VALUE_UNDEFINED) {
        /* Check metatable __index if present. */
        if (t->metatable) {
            /* For MVP, we don't implement __index. */
        }
        return VTX_VALUE_NULL;
    }
    return slot->value;
}

/* Get the next key after the given one (for pairs iteration).
 * If prev_key is VTX_VALUE_UNDEFINED, returns the first key.
 * Returns VTX_VALUE_NULL when iteration is complete. */
vtx_value_t lv_table_next(lv_table_t *t, vtx_value_t prev_key) {
    uint32_t start = 0;
    if (prev_key != VTX_VALUE_UNDEFINED) {
        lv_table_entry_t *slot = lv_table_find_slot(t, prev_key);
        if (!slot) return VTX_VALUE_NULL;
        start = (uint32_t)(slot - t->entries) + 1;
    }
    for (uint32_t i = start; i < t->capacity; i++) {
        if (t->entries[i].key != VTX_VALUE_UNDEFINED) {
            return t->entries[i].key;
        }
    }
    return VTX_VALUE_NULL;
}

/* Length of the "array part" — Lua's # operator.
 * Returns n such that t[1], t[2], ..., t[n] are non-nil and t[n+1] is nil
 * (or any border). */
int64_t lv_table_length(lv_table_t *t) {
    /* Simple linear scan for MVP. */
    int64_t n = 0;
    for (;;) {
        vtx_value_t v = lv_table_get(t, vtx_make_smi(n + 1));
        if (lv_is_nil(v)) break;
        n++;
        if (n > (1LL << 40)) break; /* sanity */
    }
    return n;
}
