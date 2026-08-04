/* lv_runtime.c — LuaVortex runtime implementation. */

#include "lv_runtime.h"
#include "lv_codegen.h"
#include "lv_stdlib.h"
#include "lv_eval.h"
#include <stdarg.h>
#include <setjmp.h>

/* Thread-local pointer to the current LuaVortex runtime. Set by
 * lv_runtime_run() before calling vtx_runtime_run(), read by the
 * patched VORTEX interpreter to dispatch extended CALL_RUNTIME
 * opcodes (lua_fn_id >= 100) back into the LuaVortex runtime. */
_Thread_local lv_runtime_t *g_lv_runtime = NULL;

/* ---- Lifecycle ---- */
int lv_runtime_create(lv_runtime_t *rt) {
    memset(rt, 0, sizeof(*rt));
    if (vtx_runtime_create(&rt->vrt) != 0) {
        return -1;
    }
    rt->globals = lv_table_new(32);
    rt->current_env = rt->globals;
    rt->last_return = VTX_VALUE_NULL;
    lv_stdlib_register(rt);
    return 0;
}

void lv_runtime_destroy(lv_runtime_t *rt) {
    if (!rt) return;
    /* Free interned strings. */
    if (rt->strings) {
        for (int i = 0; i < rt->nstrings; i++) {
            if (rt->strings[i]) lv_string_free(rt->strings[i]);
        }
        lv_free(rt->strings);
    }
    /* Free registered protos. Note: protos may be owned by the AST,
     * which is freed separately. We don't free them here to avoid
     * double-free. */
    if (rt->protos) lv_free(rt->protos);
    /* Free compiled bytecode modules (owned by the runtime). */
    if (rt->proto_bytecode) {
        for (int i = 0; i < rt->nproto_bytecode; i++) {
            if (rt->proto_bytecode[i]) {
                vtx_bytecode_t *bc = rt->proto_bytecode[i];
                if (bc->code) lv_free((void *)bc->code);
                if (bc->constant_pool) lv_free(bc->constant_pool);
                lv_free(bc);
            }
        }
        lv_free(rt->proto_bytecode);
    }
    /* Free globals table. */
    if (rt->globals) lv_table_free(rt->globals);
    if (rt->error_msg) lv_free(rt->error_msg);
    /* Destroy the VORTEX runtime. */
    vtx_runtime_destroy(&rt->vrt);
}

vtx_value_t lv_runtime_run(lv_runtime_t *rt, const vtx_bytecode_t *bc) {
    /* Set the thread-local so the patched VORTEX interpreter can call
     * back into the LuaVortex runtime for extended CALL_RUNTIME. */
    lv_runtime_t *prev = g_lv_runtime;
    g_lv_runtime = rt;
    /* Set up the env as the first argument (local 0). */
    vtx_value_t env_val = lv_make_table_val(rt->current_env);
    /* Set up an error handler so Lua-level errors longjmp back here
     * instead of crashing. */
    jmp_buf jb;
    char *prev_msg = rt->error_msg;
    rt->error_msg = NULL;
    rt->error_jmp = &jb;
    vtx_value_t result;
    if (setjmp(jb) == 0) {
        result = vtx_runtime_run_with_args(&rt->vrt, bc, &env_val, 1);
    } else {
        /* A Lua error occurred. Print it to stderr. */
        result = VTX_VALUE_NULL;
    }
    rt->error_jmp = NULL;
    if (rt->error_msg && !prev_msg) {
        fprintf(stderr, "luavortex: %s\n", rt->error_msg);
        lv_free(rt->error_msg);
        rt->error_msg = NULL;
    } else {
        rt->error_msg = prev_msg;
    }
    g_lv_runtime = prev;
    return result;
}

/* ---- String interning ---- */
vtx_value_t lv_runtime_intern_string(lv_runtime_t *rt, const char *data, size_t len) {
    /* Linear search for MVP. A hash table would be faster. */
    for (int i = 0; i < rt->nstrings; i++) {
        lv_string_t *s = rt->strings[i];
        if (s->len == len && memcmp(s->data, data, len) == 0) {
            return lv_make_string_val(s);
        }
    }
    /* Not found — create and intern. */
    if (rt->nstrings + 1 > rt->cap_strings) {
        rt->cap_strings = rt->cap_strings ? rt->cap_strings * 2 : 64;
        rt->strings = lv_realloc(rt->strings, sizeof(lv_string_t *) * rt->cap_strings);
    }
    lv_string_t *s = lv_string_new(data, len);
    rt->strings[rt->nstrings++] = s;
    return lv_make_string_val(s);
}

/* ---- Function prototype registration ---- */
int lv_runtime_register_proto(lv_runtime_t *rt, lv_func_proto_t *proto) {
    if (rt->nprotos + 1 > rt->cap_protos) {
        rt->cap_protos = rt->cap_protos ? rt->cap_protos * 2 : 16;
        rt->protos = lv_realloc(rt->protos, sizeof(lv_func_proto_t *) * rt->cap_protos);
    }
    int id = rt->nprotos++;
    rt->protos[id] = proto;
    return id;
}

vtx_value_t lv_runtime_create_closure(lv_runtime_t *rt, int proto_id) {
    if (proto_id < 0 || proto_id >= rt->nprotos) {
        return VTX_VALUE_NULL;
    }
    lv_function_t *f = lv_function_new_lua(proto_id);
    f->rt = rt;
    return lv_make_function_val(f);
}

/* Associate compiled bytecode with a proto. Grows the table as needed. */
void lv_runtime_set_proto_bytecode(lv_runtime_t *rt, int proto_id, vtx_bytecode_t *bc) {
    if (proto_id < 0) return;
    /* Grow the array if needed (sparse; indexed by proto_id). */
    if (proto_id >= rt->cap_proto_bytecode) {
        int new_cap = rt->cap_proto_bytecode ? rt->cap_proto_bytecode : 16;
        while (new_cap <= proto_id) new_cap *= 2;
        rt->proto_bytecode = lv_realloc(rt->proto_bytecode, sizeof(vtx_bytecode_t *) * new_cap);
        for (int i = rt->cap_proto_bytecode; i < new_cap; i++) {
            rt->proto_bytecode[i] = NULL;
        }
        rt->cap_proto_bytecode = new_cap;
    }
    if (proto_id >= rt->nproto_bytecode) rt->nproto_bytecode = proto_id + 1;
    rt->proto_bytecode[proto_id] = bc;
}

vtx_bytecode_t *lv_runtime_get_proto_bytecode(lv_runtime_t *rt, int proto_id) {
    if (proto_id < 0 || proto_id >= rt->nproto_bytecode) return NULL;
    return rt->proto_bytecode[proto_id];
}

/* ---- Lua function call ----
 * For MVP, calling a Lua closure means:
 *   1. Look up the proto by ID.
 *   2. Compile the proto's body to VORTEX bytecode (cached).
 *   3. Run it on the VORTEX runtime with the given args.
 *
 * This is the path that exercises the VORTEX backend for nested
 * function calls. The compiled bytecode is cached in the proto's
 * user-data field.
 *
 * For native functions, we just call the C function pointer directly. */
vtx_value_t lv_runtime_call(lv_runtime_t *rt, vtx_value_t fn_val,
                             vtx_value_t *args, int nargs) {
    if (!lv_is_function(fn_val)) {
        lv_error(rt, "attempt to call a %s value", lv_type_name(fn_val));
        return VTX_VALUE_NULL;
    }
    lv_function_t *fn = (lv_function_t *)vtx_heap_ptr(fn_val);
    if (fn->is_native) {
        /* Set the thread-local runtime so native functions can access
         * it via g_cur_rt (used when user_data is NULL or for string
         * interning inside the stdlib). */
        lv_eval_set_runtime(rt);
        return fn->u.native.fn(nargs, args, fn->u.native.user_data);
    }
    /* Lua closure. Prefer compiled bytecode if available. */
    int proto_id = fn->u.proto_id;
    if (proto_id < 0 || proto_id >= rt->nprotos) {
        lv_error(rt, "invalid function prototype id %d", proto_id);
        return VTX_VALUE_NULL;
    }
    lv_func_proto_t *proto = rt->protos[proto_id];

    /* If we have compiled bytecode, run it on the VORTEX runtime. */
    vtx_bytecode_t *bc = (fn->compiled_bc) ? fn->compiled_bc
                       : lv_runtime_get_proto_bytecode(rt, proto_id);
    if (bc) {
        /* Create a new scope table for this invocation, with __parent
         * set to the closure's captured env (or the global env). */
        lv_table_t *scope = lv_table_new(8);
        vtx_value_t parent_key = lv_runtime_intern_string(rt, "__parent", 8);
        vtx_value_t parent_val = fn->captured_env
            ? lv_make_table_val(fn->captured_env)
            : lv_make_table_val(rt->current_env);
        lv_table_set(scope, parent_key, parent_val);

        /* Bind parameters into the scope table. */
        for (int i = 0; i < proto->nparams && i < nargs; i++) {
            vtx_value_t pname = lv_runtime_intern_string(rt, proto->params[i],
                                                          strlen(proto->params[i]));
            lv_table_set(scope, pname, args[i]);
        }

        /* Run the bytecode with the scope table as local 0. */
        vtx_value_t scope_val = lv_make_table_val(scope);
        lv_runtime_t *prev_rt = g_lv_runtime;
        g_lv_runtime = rt;
        lv_eval_set_runtime(rt);
        vtx_value_t result = vtx_runtime_run_with_args(&rt->vrt, bc, &scope_val, 1);
        g_lv_runtime = prev_rt;
        return result;
    }

    /* Fallback: tree-walker (deprecated path, kept for safety). */
    return lv_runtime_eval_proto_scope(rt, proto, args, nargs, fn->enclosing_scope);
}

/* ---- Extended CALL_RUNTIME dispatch ---- */
vtx_value_t lv_runtime_dispatch(lv_runtime_t *rt, uint16_t lua_fn_id,
                                 vtx_value_t *argv, int arg_count) {
    return lv_stdlib_dispatch(rt, lua_fn_id, argv, arg_count);
}

/* ---- Error handling ---- */
void lv_error(lv_runtime_t *rt, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (rt->error_msg) lv_free(rt->error_msg);
    rt->error_msg = lv_strdup(buf);
    if (rt->error_jmp) {
        longjmp(*rt->error_jmp, 1);
    } else {
        fprintf(stderr, "luavortex: uncaught error: %s\n", buf);
        exit(1);
    }
}

const char *lv_runtime_last_error(const lv_runtime_t *rt) {
    return rt->error_msg;
}

/* ---- Memory helpers (declared in lv.h) ---- */
void *lv_alloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "luavortex: out of memory\n");
        exit(1);
    }
    return p;
}

void *lv_realloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) {
        fprintf(stderr, "luavortex: out of memory\n");
        exit(1);
    }
    return q;
}

void lv_free(void *p) {
    free(p);
}

char *lv_strdup(const char *s) {
    size_t n = strlen(s);
    char *p = lv_alloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *lv_strndup(const char *s, size_t n) {
    char *p = lv_alloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
