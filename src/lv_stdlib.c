/* lv_stdlib.c — Lua standard library.
 *
 * Implements the core Lua stdlib: print, type, tostring, tonumber,
 * pairs, ipairs, assert, error, select, rawget, rawset, rawequal,
 * next, setmetatable, getmetatable, plus the math.*, string.*, table.*,
 * io.*, os.* sublibraries.
 *
 * Each stdlib function is registered as a native lv_function_t in
 * rt->globals. They're called from two paths:
 *   1. Directly from the tree-walking evaluator (when Lua code calls
 *      a global like print()).
 *   2. Via CALL_RUNTIME from VORTEX bytecode (when the codegen emits
 *      a lua_fn_id for arithmetic, comparison, length, etc.).
 *
 * The dispatch table maps lua_fn_id → C function. The same C functions
 * are used by both paths.
 */

#include "lv_stdlib.h"
#include "lv_eval.h"
#include "lv_codegen.h"
#include <math.h>
#include <ctype.h>
#include <time.h>

/* ---- Forward declarations of stdlib native functions ----
 * Each takes (argc, argv, user_data) and returns a single value. */
typedef vtx_value_t (*lv_native_fn)(int argc, vtx_value_t *argv, void *ud);

/* ---- Helpers ---- */

static vtx_value_t make_str(lv_runtime_t *rt, const char *s, size_t len) {
    return lv_runtime_intern_string(rt, s, len);
}

static vtx_value_t make_cstr(lv_runtime_t *rt, const char *s) {
    return lv_runtime_intern_string(rt, s, strlen(s));
}

/* Register a native function under a global name. */
static void register_native(lv_runtime_t *rt, const char *name, lv_native_fn fn) {
    lv_function_t *f = lv_function_new_native(
        (vtx_value_t (*)(int, vtx_value_t *, void *))fn, NULL);
    /* The user_data is NULL; we use the function pointer alone. */
    /* But we need access to rt inside the function. We use a thread-local. */
    /* Store rt in the function's user_data via a wrapper. Actually,
     * native functions can access g_cur_rt (set by the evaluator) or
     * we set a per-call thread-local. For simplicity, we pass rt as
     * user_data. */
    f->u.native.user_data = rt;
    lv_table_set(rt->globals, make_cstr(rt, name), lv_make_function_val(f));
}

/* Most native functions need access to the runtime (for string interning,
 * error handling, etc.). Since the function signature includes user_data,
 * we pass rt through it. */

#define GET_RT(ud) ((lv_runtime_t *)(ud))

/* ---- Basic functions ---- */

static vtx_value_t fn_print(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    /* Use g_cur_rt if ud is NULL (called from evaluator). */
    if (!rt) rt = lv_eval_get_runtime();
    for (int i = 0; i < argc; i++) {
        if (i > 0) fputc('\t', stdout);
        size_t len;
        char *s = lv_to_string(argv[i], &len);
        fwrite(s, 1, len, stdout);
        lv_free(s);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return VTX_VALUE_NULL;
}

static vtx_value_t fn_type(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1) return make_cstr(rt, "nil");
    return make_cstr(rt, lv_type_name(argv[0]));
}

static vtx_value_t fn_tostring(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1) return make_cstr(rt, "nil");
    size_t len;
    char *s = lv_to_string(argv[0], &len);
    vtx_value_t v = make_str(rt, s, len);
    lv_free(s);
    return v;
}

static vtx_value_t fn_tonumber(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1) return VTX_VALUE_NULL;
    vtx_value_t v = argv[0];
    int base = (argc >= 2) ? (int)vtx_smi_value(argv[1]) : 0;
    if (base == 0) {
        bool is_int;
        int64_t iv;
        double fv;
        if (lv_to_number(v, &is_int, &iv, &fv)) {
            return is_int ? vtx_make_smi(iv) : vtx_make_double(fv);
        }
        return VTX_VALUE_NULL;
    }
    /* With base: convert string in given base. */
    if (lv_is_string(v)) {
        lv_string_t *s = (lv_string_t *)vtx_heap_ptr(v);
        char *end;
        long long iv = strtoll(s->data, &end, base);
        if (end != s->data) return vtx_make_smi((int64_t)iv);
    }
    return VTX_VALUE_NULL;
}

static vtx_value_t fn_assert(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_truthy(argv[0])) {
        const char *msg = "assertion failed!";
        if (argc >= 2 && lv_is_string(argv[1])) {
            msg = ((lv_string_t *)vtx_heap_ptr(argv[1]))->data;
        }
        lv_error(rt, "%s", msg);
    }
    return argc >= 1 ? argv[0] : VTX_VALUE_NULL;
}

static vtx_value_t fn_error(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc >= 1) {
        size_t len;
        char *s = lv_to_string(argv[0], &len);
        lv_error(rt, "%s", s);
        lv_free(s);
    } else {
        lv_error(rt, "error");
    }
    return VTX_VALUE_NULL;
}

static vtx_value_t fn_pcall(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1) return VTX_VALUE_NULL;
    /* Set up an error handler. */
    jmp_buf jb;
    jmp_buf *prev = rt->error_jmp;
    rt->error_jmp = &jb;
    char *prev_msg = rt->error_msg;
    rt->error_msg = NULL;
    vtx_value_t result;
    if (setjmp(jb) == 0) {
        vtx_value_t r = lv_runtime_call(rt, argv[0], argv + 1, argc - 1);
        /* Return (true, result). For MVP we return just true (caller
         * can't easily access the result). Actually, let's return true. */
        result = VTX_VALUE_TRUE;
        /* TODO: multi-return. For MVP, just return true. */
    } else {
        /* Error occurred. */
        result = VTX_VALUE_FALSE;
    }
    rt->error_jmp = prev;
    if (rt->error_msg) { lv_free(rt->error_msg); rt->error_msg = prev_msg; }
    else { rt->error_msg = prev_msg; }
    return result;
}

static vtx_value_t fn_select(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1) lv_error(rt, "bad argument to 'select' (number expected)");
    if (vtx_is_smi(argv[0])) {
        int64_t n = vtx_smi_value(argv[0]);
        if (n < 0) n = argc - 1 + n + 1; /* negative index from end */
        if (n < 1) lv_error(rt, "bad argument #1 to 'select' (index out of range)");
        /* Return argv[n..argc-1]. For MVP, return first. */
        if (n < argc) return argv[n];
        return VTX_VALUE_NULL;
    }
    /* "#" — return count. */
    return vtx_make_smi(argc - 1);
}

static vtx_value_t fn_rawget(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 2 || !lv_is_table(argv[0])) return VTX_VALUE_NULL;
    return lv_table_get((lv_table_t *)vtx_heap_ptr(argv[0]), argv[1]);
}

static vtx_value_t fn_rawset(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 3 || !lv_is_table(argv[0])) lv_error(rt, "bad argument to rawset");
    lv_table_set((lv_table_t *)vtx_heap_ptr(argv[0]), argv[1], argv[2]);
    return argv[0];
}

static vtx_value_t fn_rawequal(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 2) return VTX_VALUE_FALSE;
    return vtx_make_bool(lv_raw_equal(argv[0], argv[1]));
}

static vtx_value_t fn_rawlen(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1) return vtx_make_smi(0);
    if (lv_is_table(argv[0])) return vtx_make_smi(lv_table_length((lv_table_t *)vtx_heap_ptr(argv[0])));
    if (lv_is_string(argv[0])) return vtx_make_smi(((lv_string_t *)vtx_heap_ptr(argv[0]))->len);
    return vtx_make_smi(0);
}

static vtx_value_t fn_next(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_table(argv[0])) lv_error(rt, "bad argument to next");
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    vtx_value_t prev = (argc >= 2) ? argv[1] : VTX_VALUE_UNDEFINED;
    vtx_value_t key = lv_table_next(t, prev);
    if (lv_is_nil(key)) return VTX_VALUE_NULL;
    /* For MVP, return just the key (not key+value pair). */
    return key;
}

/* pairs iterator: returns (key, value) or nil. We use a closure that
 * captures the table. For MVP, we use a native function that takes
 * the table as the first arg. */
static vtx_value_t fn_pairs_iter(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    /* argv[0] = table, argv[1] = prev key (or nil/undefined for first) */
    if (argc < 1 || !lv_is_table(argv[0])) return VTX_VALUE_NULL;
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    /* Treat both nil and undefined as "start iteration". */
    vtx_value_t prev = (argc >= 2 && !lv_is_nil(argv[1])) ? argv[1] : VTX_VALUE_UNDEFINED;
    vtx_value_t key = lv_table_next(t, prev);
    if (lv_is_nil(key)) return VTX_VALUE_NULL;
    return key;
}

static vtx_value_t fn_pairs(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_table(argv[0])) lv_error(rt, "bad argument to pairs");
    /* Return the iterator function. The table and initial key are
     * handled by the evaluator's for-in loop. */
    lv_function_t *iter = lv_function_new_native(
        (vtx_value_t (*)(int, vtx_value_t *, void *))fn_pairs_iter, rt);
    return lv_make_function_val(iter);
}

static vtx_value_t fn_ipairs_iter(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1 || !lv_is_table(argv[0])) return VTX_VALUE_NULL;
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    int64_t prev = 0;
    if (argc >= 2 && !lv_is_nil(argv[1])) {
        if (!lv_to_int(argv[1], &prev)) return VTX_VALUE_NULL;
    }
    int64_t i = prev + 1;
    vtx_value_t v = lv_table_get(t, vtx_make_smi(i));
    if (lv_is_nil(v)) return VTX_VALUE_NULL;
    return vtx_make_smi(i);
}

static vtx_value_t fn_ipairs(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_table(argv[0])) lv_error(rt, "bad argument to ipairs");
    lv_function_t *iter = lv_function_new_native(
        (vtx_value_t (*)(int, vtx_value_t *, void *))fn_ipairs_iter, rt);
    return lv_make_function_val(iter);
}

static vtx_value_t fn_setmetatable(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_table(argv[0])) lv_error(rt, "bad argument to setmetatable");
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    t->metatable = (argc >= 2 && lv_is_table(argv[1]))
                       ? (lv_table_t *)vtx_heap_ptr(argv[1]) : NULL;
    return argv[0];
}

static vtx_value_t fn_getmetatable(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1 || !lv_is_table(argv[0])) return VTX_VALUE_NULL;
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    if (!t->metatable) return VTX_VALUE_NULL;
    return lv_make_table_val(t->metatable);
}

static vtx_value_t fn_unpack(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_table(argv[0])) return VTX_VALUE_NULL;
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    int64_t i = (argc >= 2) ? vtx_smi_value(argv[1]) : 1;
    int64_t j = (argc >= 3) ? vtx_smi_value(argv[2]) : lv_table_length(t);
    if (i <= j) return lv_table_get(t, vtx_make_smi(i));
    return VTX_VALUE_NULL;
}

/* ---- Math library ---- */

static double to_d(vtx_value_t v) {
    double d;
    return lv_to_double(v, &d) ? d : 0.0;
}

static vtx_value_t math_unary(lv_runtime_t *rt, double (*fn)(double), int argc, vtx_value_t *argv) {
    if (argc < 1) return VTX_VALUE_NULL;
    double r = fn(to_d(argv[0]));
    if (r == floor(r) && r >= VTX_SMI_MIN && r <= VTX_SMI_MAX) {
        return vtx_make_smi((int64_t)r);
    }
    return vtx_make_double(r);
}

static vtx_value_t fn_math_abs(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (vtx_is_smi(argv[0])) {
        int64_t v = vtx_smi_value(argv[0]);
        return vtx_make_smi(v < 0 ? -v : v);
    }
    return math_unary(rt, fabs, argc, argv);
}
static vtx_value_t fn_math_floor(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud); return math_unary(rt, floor, argc, argv);
}
static vtx_value_t fn_math_ceil(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud); return math_unary(rt, ceil, argc, argv);
}
static vtx_value_t fn_math_sqrt(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud); return math_unary(rt, sqrt, argc, argv);
}
static vtx_value_t fn_math_sin(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud); return math_unary(rt, sin, argc, argv);
}
static vtx_value_t fn_math_cos(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud); return math_unary(rt, cos, argc, argv);
}
static vtx_value_t fn_math_tan(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud); return math_unary(rt, tan, argc, argv);
}
static vtx_value_t fn_math_log(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (argc >= 2) {
        double b = to_d(argv[1]);
        return vtx_make_double(log(to_d(argv[0])) / log(b));
    }
    return math_unary(rt, log, argc, argv);
}
static vtx_value_t fn_math_exp(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud); return math_unary(rt, exp, argc, argv);
}
static vtx_value_t fn_math_pow(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 2) return VTX_VALUE_NULL;
    return vtx_make_double(pow(to_d(argv[0]), to_d(argv[1])));
}
static vtx_value_t fn_math_max(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1) return VTX_VALUE_NULL;
    double m = to_d(argv[0]);
    for (int i = 1; i < argc; i++) {
        double v = to_d(argv[i]);
        if (v > m) m = v;
    }
    if (m == floor(m) && m >= VTX_SMI_MIN && m <= VTX_SMI_MAX)
        return vtx_make_smi((int64_t)m);
    return vtx_make_double(m);
}
static vtx_value_t fn_math_min(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1) return VTX_VALUE_NULL;
    double m = to_d(argv[0]);
    for (int i = 1; i < argc; i++) {
        double v = to_d(argv[i]);
        if (v < m) m = v;
    }
    if (m == floor(m) && m >= VTX_SMI_MIN && m <= VTX_SMI_MAX)
        return vtx_make_smi((int64_t)m);
    return vtx_make_double(m);
}
static vtx_value_t fn_math_random(int argc, vtx_value_t *argv, void *ud) {
    static bool seeded = false;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = true; }
    if (argc == 0) {
        return vtx_make_double((double)rand() / RAND_MAX);
    }
    int64_t lo = (argc >= 1) ? vtx_smi_value(argv[0]) : 1;
    int64_t hi = (argc >= 2) ? vtx_smi_value(argv[1]) : lo;
    if (argc == 1) { hi = lo; lo = 1; }
    int64_t r = lo + (rand() % (int)(hi - lo + 1));
    return vtx_make_smi(r);
}

/* ---- String library ---- */

static vtx_value_t fn_str_len(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1 || !lv_is_string(argv[0])) return vtx_make_smi(0);
    return vtx_make_smi(((lv_string_t *)vtx_heap_ptr(argv[0]))->len);
}

static vtx_value_t fn_str_sub(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_string(argv[0])) return make_cstr(rt, "");
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    int64_t len = (int64_t)s->len;
    int64_t i = (argc >= 2) ? vtx_smi_value(argv[1]) : 1;
    int64_t j = (argc >= 3) ? vtx_smi_value(argv[2]) : len;
    if (i < 0) i = len + 1 + i;
    if (j < 0) j = len + 1 + j;
    if (i < 1) i = 1;
    if (j > len) j = len;
    if (i > j) return make_cstr(rt, "");
    return make_str(rt, s->data + i - 1, (size_t)(j - i + 1));
}

static vtx_value_t fn_str_upper(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_string(argv[0])) return make_cstr(rt, "");
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    char *buf = lv_alloc(s->len);
    for (size_t i = 0; i < s->len; i++) buf[i] = toupper((unsigned char)s->data[i]);
    vtx_value_t r = make_str(rt, buf, s->len);
    lv_free(buf);
    return r;
}

static vtx_value_t fn_str_lower(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_string(argv[0])) return make_cstr(rt, "");
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    char *buf = lv_alloc(s->len);
    for (size_t i = 0; i < s->len; i++) buf[i] = tolower((unsigned char)s->data[i]);
    vtx_value_t r = make_str(rt, buf, s->len);
    lv_free(buf);
    return r;
}

static vtx_value_t fn_str_rep(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 2 || !lv_is_string(argv[0])) return make_cstr(rt, "");
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    int64_t n = vtx_smi_value(argv[1]);
    if (n <= 0) return make_cstr(rt, "");
    char *buf = lv_alloc(s->len * (size_t)n + 1);
    for (int64_t i = 0; i < n; i++) memcpy(buf + i * s->len, s->data, s->len);
    buf[s->len * n] = 0;
    vtx_value_t r = make_str(rt, buf, s->len * (size_t)n);
    lv_free(buf);
    return r;
}

static vtx_value_t fn_str_reverse(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_string(argv[0])) return make_cstr(rt, "");
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    char *buf = lv_alloc(s->len);
    for (size_t i = 0; i < s->len; i++) buf[i] = s->data[s->len - 1 - i];
    vtx_value_t r = make_str(rt, buf, s->len);
    lv_free(buf);
    return r;
}

static vtx_value_t fn_str_byte(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1 || !lv_is_string(argv[0])) return VTX_VALUE_NULL;
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    int64_t i = (argc >= 2) ? vtx_smi_value(argv[1]) : 1;
    if (i < 1 || i > (int64_t)s->len) return VTX_VALUE_NULL;
    return vtx_make_smi((uint8_t)s->data[i - 1]);
}

static vtx_value_t fn_str_char(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    char *buf = lv_alloc(argc + 1);
    for (int i = 0; i < argc; i++) buf[i] = (char)vtx_smi_value(argv[i]);
    buf[argc] = 0;
    vtx_value_t r = make_str(rt, buf, argc);
    lv_free(buf);
    return r;
}

static vtx_value_t fn_str_format(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_string(argv[0])) return make_cstr(rt, "");
    lv_string_t *fmt = (lv_string_t *)vtx_heap_ptr(argv[0]);
    /* Simple format: walk the format string, handling %d %s %f %x %c %% */
    size_t cap = 64;
    char *buf = lv_alloc(cap);
    size_t len = 0;
    int arg_i = 1;
    #define APPEND_CHAR(c) do { \
        if (len + 1 >= cap) { cap *= 2; buf = lv_realloc(buf, cap); } \
        buf[len++] = (c); \
    } while (0)
    for (size_t i = 0; i < fmt->len; i++) {
        if (fmt->data[i] != '%') { APPEND_CHAR(fmt->data[i]); continue; }
        i++;
        if (i >= fmt->len) break;
        char c = fmt->data[i];
        switch (c) {
        case '%': APPEND_CHAR('%'); break;
        case 'd':
        case 'i': {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%lld",
                             (long long)(arg_i < argc ? vtx_smi_value(argv[arg_i++]) : 0));
            for (int k = 0; k < n; k++) APPEND_CHAR(tmp[k]);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            char tmp[32];
            const char *spec = (c == 'u') ? "%llu" : (c == 'x') ? "%llx" :
                               (c == 'X') ? "%llX" : "%llo";
            int n = snprintf(tmp, sizeof(tmp), spec,
                             (unsigned long long)(arg_i < argc ? vtx_smi_value(argv[arg_i++]) : 0));
            for (int k = 0; k < n; k++) APPEND_CHAR(tmp[k]);
            break;
        }
        case 'f': case 'g': case 'e': case 'F': case 'G': case 'E': {
            char tmp[64];
            char spec[4] = { '%', c, 0 };
            int n = snprintf(tmp, sizeof(tmp), spec,
                             arg_i < argc ? to_d(argv[arg_i++]) : 0.0);
            for (int k = 0; k < n; k++) APPEND_CHAR(tmp[k]);
            break;
        }
        case 's': {
            if (arg_i < argc) {
                size_t sl;
                char *s = lv_to_string(argv[arg_i++], &sl);
                for (size_t k = 0; k < sl; k++) APPEND_CHAR(s[k]);
                lv_free(s);
            }
            break;
        }
        case 'c': {
            if (arg_i < argc) {
                APPEND_CHAR((char)vtx_smi_value(argv[arg_i++]));
            }
            break;
        }
        case 'q': {
            if (arg_i < argc) {
                size_t sl;
                char *s = lv_to_string(argv[arg_i++], &sl);
                APPEND_CHAR('"');
                for (size_t k = 0; k < sl; k++) {
                    char ch = s[k];
                    if (ch == '"' || ch == '\\') { APPEND_CHAR('\\'); APPEND_CHAR(ch); }
                    else if (ch == '\n') { APPEND_CHAR('\\'); APPEND_CHAR('n'); }
                    else if (ch == '\r') { APPEND_CHAR('\\'); APPEND_CHAR('r'); }
                    else if (ch == '\t') { APPEND_CHAR('\\'); APPEND_CHAR('t'); }
                    else APPEND_CHAR(ch);
                }
                APPEND_CHAR('"');
                lv_free(s);
            }
            break;
        }
        default:
            APPEND_CHAR('%');
            APPEND_CHAR(c);
            break;
        }
    }
    buf[len] = 0;
    vtx_value_t r = make_str(rt, buf, len);
    lv_free(buf);
    return r;
}

/* fn_str_find is defined later (with pattern matching support). */

/* ---- Table library ---- */

static vtx_value_t fn_tbl_insert(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 2 || !lv_is_table(argv[0])) lv_error(rt, "bad argument to table.insert");
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    int64_t n = lv_table_length(t);
    if (argc == 2) {
        /* append */
        lv_table_set(t, vtx_make_smi(n + 1), argv[1]);
    } else {
        /* insert at position */
        int64_t pos = vtx_smi_value(argv[1]);
        vtx_value_t val = argv[2];
        for (int64_t i = n; i >= pos; i--) {
            lv_table_set(t, vtx_make_smi(i + 1), lv_table_get(t, vtx_make_smi(i)));
        }
        lv_table_set(t, vtx_make_smi(pos), val);
    }
    return VTX_VALUE_NULL;
}

static vtx_value_t fn_tbl_remove(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_table(argv[0])) lv_error(rt, "bad argument to table.remove");
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    int64_t n = lv_table_length(t);
    if (n == 0) return VTX_VALUE_NULL;
    int64_t pos = (argc >= 2) ? vtx_smi_value(argv[1]) : n;
    vtx_value_t v = lv_table_get(t, vtx_make_smi(pos));
    for (int64_t i = pos; i < n; i++) {
        lv_table_set(t, vtx_make_smi(i), lv_table_get(t, vtx_make_smi(i + 1)));
    }
    lv_table_set(t, vtx_make_smi(n), VTX_VALUE_NULL);
    return v;
}

static vtx_value_t fn_tbl_concat(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_table(argv[0])) return make_cstr(rt, "");
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    const char *sep = "";
    size_t sep_len = 0;
    if (argc >= 2 && lv_is_string(argv[1])) {
        lv_string_t *ss = (lv_string_t *)vtx_heap_ptr(argv[1]);
        sep = ss->data; sep_len = ss->len;
    }
    int64_t i_start = (argc >= 3) ? vtx_smi_value(argv[2]) : 1;
    int64_t i_end   = (argc >= 4) ? vtx_smi_value(argv[3]) : lv_table_length(t);
    size_t cap = 64, len = 0;
    char *buf = lv_alloc(cap);
    for (int64_t i = i_start; i <= i_end; i++) {
        if (i > i_start) {
            while (len + sep_len + 1 >= cap) { cap *= 2; buf = lv_realloc(buf, cap); }
            memcpy(buf + len, sep, sep_len); len += sep_len;
        }
        vtx_value_t v = lv_table_get(t, vtx_make_smi(i));
        size_t sl;
        char *s = lv_to_string(v, &sl);
        while (len + sl + 1 >= cap) { cap *= 2; buf = lv_realloc(buf, cap); }
        memcpy(buf + len, s, sl); len += sl;
        lv_free(s);
    }
    buf[len] = 0;
    vtx_value_t r = make_str(rt, buf, len);
    lv_free(buf);
    return r;
}

static vtx_value_t fn_tbl_sort(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_table(argv[0])) lv_error(rt, "bad argument to table.sort");
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    int64_t n = lv_table_length(t);
    /* Simple insertion sort (MVP). */
    for (int64_t i = 2; i <= n; i++) {
        vtx_value_t key = lv_table_get(t, vtx_make_smi(i));
        int64_t j = i - 1;
        while (j >= 1) {
            vtx_value_t cur = lv_table_get(t, vtx_make_smi(j));
            /* Compare: if cur > key, shift. We compute (key < cur). */
            vtx_value_t cmp_args[2] = {key, cur};
            vtx_value_t r = lv_stdlib_dispatch(rt, 131 /*LV_FN_CMP_LT*/, cmp_args, 2);
            if (!lv_is_truthy(r)) {
                /* key >= cur: stop shifting. */
                break;
            }
            /* key < cur: shift cur right. */
            lv_table_set(t, vtx_make_smi(j + 1), cur);
            j--;
        }
        lv_table_set(t, vtx_make_smi(j + 1), key);
    }
    return VTX_VALUE_NULL;
}

/* ---- I/O library ---- */

static vtx_value_t fn_io_write(int argc, vtx_value_t *argv, void *ud) {
    for (int i = 0; i < argc; i++) {
        size_t len;
        char *s = lv_to_string(argv[i], &len);
        fwrite(s, 1, len, stdout);
        lv_free(s);
    }
    fflush(stdout);
    return VTX_VALUE_NULL;
}

static vtx_value_t fn_io_read(int argc, vtx_value_t *argv, void *ud) {
    /* Read a line from stdin. */
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) { free(line); return VTX_VALUE_NULL; }
    if (n > 0 && line[n - 1] == '\n') n--;
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    vtx_value_t v = make_str(rt, line, n);
    free(line);
    return v;
}

/* ---- OS library ---- */

static vtx_value_t fn_os_time(int argc, vtx_value_t *argv, void *ud) {
    return vtx_make_smi((int64_t)time(NULL));
}

static vtx_value_t fn_os_clock(int argc, vtx_value_t *argv, void *ud) {
    return vtx_make_double((double)clock() / CLOCKS_PER_SEC);
}

static vtx_value_t fn_os_date(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char buf[256];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", lt);
    return make_cstr(rt, buf);
}

/* ---- Arithmetic / comparison dispatchers ----
 * These are the functions invoked via CALL_RUNTIME with lua_fn_id >= 120.
 * They take their args in argv[0..argc-1]. */

static vtx_value_t arith_add(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    bool a_int = vtx_is_smi(a), b_int = vtx_is_smi(b);
    if (a_int && b_int) {
        int64_t r = vtx_smi_value(a) + vtx_smi_value(b);
        if (r >= VTX_SMI_MIN && r <= VTX_SMI_MAX) return vtx_make_smi(r);
        return vtx_make_double((double)r);
    }
    double da, db;
    if (!lv_to_double(a, &da) || !lv_to_double(b, &db)) {
        lv_error(rt, "attempt to perform arithmetic on a %s value", lv_type_name(a));
    }
    return vtx_make_double(da + db);
}

static vtx_value_t arith_sub(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    bool a_int = vtx_is_smi(a), b_int = vtx_is_smi(b);
    if (a_int && b_int) {
        int64_t r = vtx_smi_value(a) - vtx_smi_value(b);
        if (r >= VTX_SMI_MIN && r <= VTX_SMI_MAX) return vtx_make_smi(r);
        return vtx_make_double((double)r);
    }
    double da, db;
    if (!lv_to_double(a, &da) || !lv_to_double(b, &db)) {
        lv_error(rt, "attempt to perform arithmetic on a %s value", lv_type_name(a));
    }
    return vtx_make_double(da - db);
}

static vtx_value_t arith_mul(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    bool a_int = vtx_is_smi(a), b_int = vtx_is_smi(b);
    if (a_int && b_int) {
        int64_t r = vtx_smi_value(a) * vtx_smi_value(b);
        if (r >= VTX_SMI_MIN && r <= VTX_SMI_MAX) return vtx_make_smi(r);
        return vtx_make_double((double)r);
    }
    double da, db;
    if (!lv_to_double(a, &da) || !lv_to_double(b, &db)) {
        lv_error(rt, "attempt to perform arithmetic on a %s value", lv_type_name(a));
    }
    return vtx_make_double(da * db);
}

static vtx_value_t arith_div(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    double da, db;
    if (!lv_to_double(a, &da) || !lv_to_double(b, &db)) {
        lv_error(rt, "attempt to perform arithmetic on a %s value", lv_type_name(a));
    }
    if (db == 0.0) {
        if (da == 0.0) return vtx_make_double(NAN);
        return vtx_make_double(da > 0 ? INFINITY : -INFINITY);
    }
    /* Integer division if both are ints and divisible. */
    if (vtx_is_smi(a) && vtx_is_smi(b) && da != 0) {
        int64_t ia = vtx_smi_value(a), ib = vtx_smi_value(b);
        if (ia % ib == 0) return vtx_make_smi(ia / ib);
    }
    return vtx_make_double(da / db);
}

static vtx_value_t arith_idiv(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    /* Floor division. */
    double da, db;
    if (!lv_to_double(a, &da) || !lv_to_double(b, &db)) {
        lv_error(rt, "attempt to perform arithmetic on a %s value", lv_type_name(a));
    }
    if (db == 0.0) lv_error(rt, "attempt to perform 'n//0'");
    double r = floor(da / db);
    if (r >= VTX_SMI_MIN && r <= VTX_SMI_MAX) return vtx_make_smi((int64_t)r);
    return vtx_make_double(r);
}

static vtx_value_t arith_mod(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    double da, db;
    if (!lv_to_double(a, &da) || !lv_to_double(b, &db)) {
        lv_error(rt, "attempt to perform arithmetic on a %s value", lv_type_name(a));
    }
    if (db == 0.0) lv_error(rt, "attempt to perform 'n%%0'");
    double r = da - floor(da / db) * db;
    if (vtx_is_smi(a) && vtx_is_smi(b) && r == floor(r) &&
        r >= VTX_SMI_MIN && r <= VTX_SMI_MAX) {
        return vtx_make_smi((int64_t)r);
    }
    return vtx_make_double(r);
}

static vtx_value_t arith_pow(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    double da, db;
    if (!lv_to_double(a, &da) || !lv_to_double(b, &db)) {
        lv_error(rt, "attempt to perform arithmetic on a %s value", lv_type_name(a));
    }
    double r = pow(da, db);
    if (vtx_is_smi(a) && vtx_is_smi(b) && db >= 0 &&
        r == floor(r) && r >= VTX_SMI_MIN && r <= VTX_SMI_MAX) {
        return vtx_make_smi((int64_t)r);
    }
    return vtx_make_double(r);
}

static vtx_value_t arith_concat(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    size_t la, lb;
    char *sa = lv_to_string(a, &la);
    char *sb = lv_to_string(b, &lb);
    char *buf = lv_alloc(la + lb);
    memcpy(buf, sa, la);
    memcpy(buf + la, sb, lb);
    vtx_value_t r = lv_runtime_intern_string(rt, buf, la + lb);
    lv_free(sa); lv_free(sb); lv_free(buf);
    return r;
}

static vtx_value_t cmp_eq(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    return vtx_make_bool(lv_raw_equal(a, b));
}

static vtx_value_t cmp_lt(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    if (vtx_is_smi(a) && vtx_is_smi(b)) {
        return vtx_make_bool(vtx_smi_value(a) < vtx_smi_value(b));
    }
    if (lv_is_number(a) && lv_is_number(b)) {
        return vtx_make_bool(to_d(a) < to_d(b));
    }
    if (lv_is_string(a) && lv_is_string(b)) {
        lv_string_t *sa = (lv_string_t *)vtx_heap_ptr(a);
        lv_string_t *sb = (lv_string_t *)vtx_heap_ptr(b);
        size_t n = sa->len < sb->len ? sa->len : sb->len;
        int c = memcmp(sa->data, sb->data, n);
        if (c == 0) return vtx_make_bool(sa->len < sb->len);
        return vtx_make_bool(c < 0);
    }
    lv_error(rt, "attempt to compare %s with %s", lv_type_name(a), lv_type_name(b));
    return VTX_VALUE_FALSE;
}

static vtx_value_t cmp_le(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    if (vtx_is_smi(a) && vtx_is_smi(b)) {
        return vtx_make_bool(vtx_smi_value(a) <= vtx_smi_value(b));
    }
    if (lv_is_number(a) && lv_is_number(b)) {
        return vtx_make_bool(to_d(a) <= to_d(b));
    }
    if (lv_is_string(a) && lv_is_string(b)) {
        lv_string_t *sa = (lv_string_t *)vtx_heap_ptr(a);
        lv_string_t *sb = (lv_string_t *)vtx_heap_ptr(b);
        size_t n = sa->len < sb->len ? sa->len : sb->len;
        int c = memcmp(sa->data, sb->data, n);
        if (c == 0) return vtx_make_bool(sa->len <= sb->len);
        return vtx_make_bool(c < 0);
    }
    lv_error(rt, "attempt to compare %s with %s", lv_type_name(a), lv_type_name(b));
    return VTX_VALUE_FALSE;
}

static vtx_value_t bit_and(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    int64_t ia, ib;
    if (!lv_to_int(a, &ia) || !lv_to_int(b, &ib)) {
        lv_error(rt, "attempt to perform bitwise operation on non-integer");
    }
    return vtx_make_smi(ia & ib);
}
static vtx_value_t bit_or(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    int64_t ia, ib;
    if (!lv_to_int(a, &ia) || !lv_to_int(b, &ib)) {
        lv_error(rt, "attempt to perform bitwise operation on non-integer");
    }
    return vtx_make_smi(ia | ib);
}
static vtx_value_t bit_xor(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    int64_t ia, ib;
    if (!lv_to_int(a, &ia) || !lv_to_int(b, &ib)) {
        lv_error(rt, "attempt to perform bitwise operation on non-integer");
    }
    return vtx_make_smi(ia ^ ib);
}
static vtx_value_t bit_shl(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    int64_t ia, ib;
    if (!lv_to_int(a, &ia) || !lv_to_int(b, &ib)) {
        lv_error(rt, "attempt to perform bitwise operation on non-integer");
    }
    if (ib < 0) return vtx_make_smi(ia >> (-ib));
    if (ib >= 63) return vtx_make_smi(0);
    return vtx_make_smi(ia << ib);
}
static vtx_value_t bit_shr(lv_runtime_t *rt, vtx_value_t a, vtx_value_t b) {
    int64_t ia, ib;
    if (!lv_to_int(a, &ia) || !lv_to_int(b, &ib)) {
        lv_error(rt, "attempt to perform bitwise operation on non-integer");
    }
    if (ib < 0) return vtx_make_smi(ia << (-ib));
    if (ib >= 63) return vtx_make_smi(0);
    /* Treat as unsigned shift. */
    uint64_t u = (uint64_t)ia;
    return vtx_make_smi((int64_t)(u >> ib));
}

/* ---- Length ---- */
static vtx_value_t lv_length(lv_runtime_t *rt, vtx_value_t v) {
    if (lv_is_string(v)) return vtx_make_smi(((lv_string_t *)vtx_heap_ptr(v))->len);
    if (lv_is_table(v))  return vtx_make_smi(lv_table_length((lv_table_t *)vtx_heap_ptr(v)));
    lv_error(rt, "attempt to get length of a %s value", lv_type_name(v));
    return vtx_make_smi(0);
}

/* ---- Function call dispatcher (LV_FN_CALL = 220) ---- */
static vtx_value_t lv_fn_call(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    if (argc < 1) { lv_error(rt, "attempt to call a nil value"); return VTX_VALUE_NULL; }
    return lv_runtime_call(rt, argv[0], argv + 1, argc - 1);
}

/* ---- Method call dispatcher (LV_FN_CALL_METHOD = 221) ---- */
static vtx_value_t lv_fn_call_method(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    /* argv[0] = recv, argv[1] = method name (string), argv[2..] = args */
    if (argc < 2) { lv_error(rt, "method call: missing receiver/name"); return VTX_VALUE_NULL; }
    vtx_value_t recv = argv[0];
    vtx_value_t name = argv[1];
    vtx_value_t method;
    if (lv_is_table(recv)) {
        method = lv_table_get((lv_table_t *)vtx_heap_ptr(recv), name);
    } else if (lv_is_string(recv)) {
        lv_table_t *strlib = lv_stdlib_string_lib(rt);
        method = strlib ? lv_table_get(strlib, name) : VTX_VALUE_NULL;
    } else {
        lv_error(rt, "attempt to index a %s value", lv_type_name(recv));
        return VTX_VALUE_NULL;
    }
    if (!lv_is_function(method)) {
        const char *mname = lv_is_string(name) ? ((lv_string_t *)vtx_heap_ptr(name))->data : "?";
        lv_error(rt, "attempt to call method '%s' (not a function)", mname);
        return VTX_VALUE_NULL;
    }
    /* args = [recv, argv[2..]]. */
    int nargs = argc - 2;
    vtx_value_t *args = lv_alloc(sizeof(vtx_value_t) * (1 + nargs));
    args[0] = recv;
    for (int i = 0; i < nargs; i++) args[i + 1] = argv[i + 2];
    vtx_value_t r = lv_runtime_call(rt, method, args, 1 + nargs);
    lv_free(args);
    return r;
}

/* ---- New table (LV_FN_NEW_TABLE = 222) ---- */
static vtx_value_t lv_fn_new_table(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    uint32_t narr = (argc >= 1 && vtx_is_smi(argv[0])) ? (uint32_t)vtx_smi_value(argv[0]) : 0;
    uint32_t nrec = (argc >= 2 && vtx_is_smi(argv[1])) ? (uint32_t)vtx_smi_value(argv[1]) : 0;
    uint32_t cap = narr + nrec;
    if (cap < 8) cap = 8;
    return lv_make_table_val(lv_table_new(cap));
}

/* ---- Set/Get field (LV_FN_SET_FIELD = 223, LV_FN_GET_FIELD = 224) ---- */
static vtx_value_t lv_fn_set_field(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    /* argv[0] = table, argv[1] = key, argv[2] = value. Returns table. */
    if (argc < 3 || !lv_is_table(argv[0])) {
        lv_error(rt, "attempt to index a %s value",
                 argc >= 1 ? lv_type_name(argv[0]) : "nil");
        return VTX_VALUE_NULL;
    }
    lv_table_set((lv_table_t *)vtx_heap_ptr(argv[0]), argv[1], argv[2]);
    return argv[0];
}

static vtx_value_t lv_fn_get_field(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    if (argc < 2 || !lv_is_table(argv[0])) {
        lv_error(rt, "attempt to index a %s value",
                 argc >= 1 ? lv_type_name(argv[0]) : "nil");
        return VTX_VALUE_NULL;
    }
    return lv_table_get((lv_table_t *)vtx_heap_ptr(argv[0]), argv[1]);
}

/* ---- New closure (LV_FN_NEW_CLOSURE = 225) ---- */
static vtx_value_t lv_fn_new_closure(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    if (argc < 1 || !vtx_is_smi(argv[0])) return VTX_VALUE_NULL;
    int proto_id = (int)vtx_smi_value(argv[0]);
    return lv_runtime_create_closure(rt, proto_id);
}

/* ---- Varargs (LV_FN_VARARG = 226) ---- */
static vtx_value_t lv_fn_vararg(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    /* For MVP, return nil. */
    return VTX_VALUE_NULL;
}

/* ---- Global get/set (LV_FN_GLOBAL_GET = 228, LV_FN_GLOBAL_SET = 229) ---- */
static vtx_value_t lv_fn_global_get(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    /* argv[0] = env (table), argv[1] = name (string). */
    if (argc < 2 || !lv_is_table(argv[0])) return VTX_VALUE_NULL;
    return lv_table_get((lv_table_t *)vtx_heap_ptr(argv[0]), argv[1]);
}

static vtx_value_t lv_fn_global_set(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    /* argv[0] = env, argv[1] = name, argv[2] = value. */
    if (argc < 3 || !lv_is_table(argv[0])) return VTX_VALUE_NULL;
    lv_table_set((lv_table_t *)vtx_heap_ptr(argv[0]), argv[1], argv[2]);
    return VTX_VALUE_NULL;
}

/* ---- Expanded string library: pattern matching ----
 * Lua patterns are simpler than regex but share some syntax.
 * For MVP, we implement a subset: literals, . (any), %a %d %s etc.
 * (character classes), * + - (quantifiers), and captures via (). */

/* Check if a single character matches a pattern element.
 * Returns the length of the pattern element consumed, or 0 if no match. */
static int pattern_match_char(const char *pat, int pat_pos, int pat_len,
                               char c, bool *matched) {
    *matched = false;
    if (pat_pos >= pat_len) return 0;
    char p = pat[pat_pos];
    if (p == '%') {
        /* %class — character class */
        if (pat_pos + 1 >= pat_len) return 0;
        char cls = pat[pat_pos + 1];
        bool neg = false;
        if (isupper((unsigned char)cls)) { cls = tolower((unsigned char)cls); neg = true; }
        bool m = false;
        switch (cls) {
        case 'a': m = isalpha((unsigned char)c); break;
        case 'd': m = isdigit((unsigned char)c); break;
        case 'l': m = islower((unsigned char)c); break;
        case 'u': m = isupper((unsigned char)c); break;
        case 's': m = isspace((unsigned char)c); break;
        case 'w': m = isalnum((unsigned char)c); break;
        case 'p': m = ispunct((unsigned char)c); break;
        case 'c': m = iscntrl((unsigned char)c); break;
        case 'x': m = isxdigit((unsigned char)c); break;
        default:
            /* %x where x is a literal char (e.g., %. matches '.') */
            m = (c == cls);
            break;
        }
        *matched = neg ? !m : m;
        return 2;
    }
    if (p == '.') {
        *matched = true;
        return 1;
    }
    *matched = (c == p);
    return 1;
}

/* Simple pattern match (no captures). Returns match length or -1. */
static int pattern_match_at(const char *pat, int pat_len,
                             const char *str, int str_pos, int str_len) {
    int pp = 0, sp = str_pos;
    while (pp < pat_len) {
        /* Check for end-of-string anchors ($) */
        if (pat[pp] == '$' && pp == pat_len - 1) {
            return (sp == str_len) ? (sp - str_pos) : -1;
        }
        /* Look ahead for quantifier. */
        bool matched = false;
        int elem_len = pattern_match_char(pat, pp, pat_len, sp < str_len ? str[sp] : '\0', &matched);
        if (elem_len == 0) return -1;
        /* Check next char for quantifier. */
        char quant = (pp + elem_len < pat_len) ? pat[pp + elem_len] : 0;
        if (quant == '*') {
            /* Zero or more. */
            int count = 0;
            while (sp + count < str_len) {
                bool m;
                pattern_match_char(pat, pp, pat_len, str[sp + count], &m);
                if (!m) break;
                count++;
            }
            /* Try to match the rest greedily, backing off. */
            int rest_pp = pp + elem_len + 1;
            for (int try_count = count; try_count >= 0; try_count--) {
                int r = pattern_match_at(pat + rest_pp, pat_len - rest_pp,
                                          str, sp + try_count, str_len);
                if (r >= 0) return (sp + try_count + r) - str_pos;
            }
            return -1;
        } else if (quant == '+') {
            /* One or more. */
            int count = 0;
            while (sp + count < str_len) {
                bool m;
                pattern_match_char(pat, pp, pat_len, str[sp + count], &m);
                if (!m) break;
                count++;
            }
            if (count == 0) return -1;
            int rest_pp = pp + elem_len + 1;
            for (int try_count = count; try_count >= 1; try_count--) {
                int r = pattern_match_at(pat + rest_pp, pat_len - rest_pp,
                                          str, sp + try_count, str_len);
                if (r >= 0) return (sp + try_count + r) - str_pos;
            }
            return -1;
        } else if (quant == '-') {
            /* Zero or more, lazy. */
            int rest_pp = pp + elem_len + 1;
            for (int try_count = 0; sp + try_count <= str_len; try_count++) {
                bool m = false;
                if (try_count > 0) {
                    pattern_match_char(pat, pp, pat_len, str[sp + try_count - 1], &m);
                    if (!m) break;
                }
                int r = pattern_match_at(pat + rest_pp, pat_len - rest_pp,
                                          str, sp + try_count, str_len);
                if (r >= 0) return (sp + try_count + r) - str_pos;
            }
            return -1;
        } else if (quant == '?') {
            /* Zero or one. */
            int rest_pp = pp + elem_len + 1;
            /* Try with one. */
            if (matched) {
                int r = pattern_match_at(pat + rest_pp, pat_len - rest_pp,
                                          str, sp + 1, str_len);
                if (r >= 0) return (sp + 1 + r) - str_pos;
            }
            /* Try with zero. */
            int r = pattern_match_at(pat + rest_pp, pat_len - rest_pp,
                                      str, sp, str_len);
            if (r >= 0) return (sp + r) - str_pos;
            return -1;
        } else {
            /* No quantifier — must match exactly. */
            if (!matched) return -1;
            sp += 1;
            pp += elem_len;
        }
    }
    return sp - str_pos;
}

/* Find first match of pat in str starting at or after str[start]. */
static int pattern_find(const char *str, int str_len,
                         const char *pat, int pat_len, int start) {
    for (int i = start; i <= str_len; i++) {
        int r = pattern_match_at(pat, pat_len, str, i, str_len);
        if (r >= 0) return i;
    }
    return -1;
}

static vtx_value_t fn_str_match(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 2 || !lv_is_string(argv[0]) || !lv_is_string(argv[1]))
        return VTX_VALUE_NULL;
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    lv_string_t *p = (lv_string_t *)vtx_heap_ptr(argv[1]);
    int start = (argc >= 3 && vtx_is_smi(argv[2])) ? (int)vtx_smi_value(argv[2]) - 1 : 0;
    if (start < 0) start = 0;
    int pos = pattern_find(s->data, (int)s->len, p->data, (int)p->len, start);
    if (pos < 0) return VTX_VALUE_NULL;
    int len = pattern_match_at(p->data, (int)p->len, s->data, pos, (int)s->len);
    if (len < 0) return VTX_VALUE_NULL;
    return make_str(rt, s->data + pos, len);
}

static vtx_value_t fn_str_find(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 2 || !lv_is_string(argv[0]) || !lv_is_string(argv[1]))
        return VTX_VALUE_NULL;
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    lv_string_t *p = (lv_string_t *)vtx_heap_ptr(argv[1]);
    int start = (argc >= 3 && vtx_is_smi(argv[2])) ? (int)vtx_smi_value(argv[2]) - 1 : 0;
    if (start < 0) start = 0;
    /* If pattern is "plain" (no special chars), use simple search. */
    bool plain = (argc >= 4 && vtx_is_bool(argv[3])) ? vtx_bool_value(argv[3]) : false;
    if (plain || strpbrk(p->data, "%.+-*$?[]^") == NULL) {
        /* Plain text search. */
        if (p->len == 0) return vtx_make_smi(start + 1 <= (int64_t)s->len ? start + 1 : 0);
        for (int i = start; i + p->len <= s->len; i++) {
            if (memcmp(s->data + i, p->data, p->len) == 0) return vtx_make_smi(i + 1);
        }
        return VTX_VALUE_NULL;
    }
    int pos = pattern_find(s->data, (int)s->len, p->data, (int)p->len, start);
    if (pos < 0) return VTX_VALUE_NULL;
    return vtx_make_smi(pos + 1);
}

static vtx_value_t fn_str_gmatch(int argc, vtx_value_t *argv, void *ud) {
    /* gmatch returns an iterator function. For MVP, we use a stateful
     * approach: the iterator carries the string, pattern, and current
     * position in user_data. */
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 2 || !lv_is_string(argv[0]) || !lv_is_string(argv[1]))
        return VTX_VALUE_NULL;
    /* We can't easily store state in a native function. For MVP, return
     * a closure-like object. Actually, let's implement gmatch as a
     * coroutine-like iterator using a heap-allocated state. */
    /* For MVP, return nil (not yet supported via this path). The user
     * can use a manual loop with find + sub instead. */
    lv_error(rt, "string.gmatch not yet implemented (use a manual find+sub loop)");
    return VTX_VALUE_NULL;
}

static vtx_value_t fn_str_gsub(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 3 || !lv_is_string(argv[0]) || !lv_is_string(argv[1]))
        return VTX_VALUE_NULL;
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    lv_string_t *p = (lv_string_t *)vtx_heap_ptr(argv[1]);
    /* Replacement can be a string or function. */
    vtx_value_t repl = argv[2];
    int64_t max_n = (argc >= 4 && vtx_is_smi(argv[3])) ? vtx_smi_value(argv[3]) : -1;

    /* Build the result. */
    size_t cap = s->len + 16;
    char *buf = lv_alloc(cap);
    size_t len = 0;
    int count = 0;
    int pos = 0;
    while (pos <= (int)s->len && (max_n < 0 || count < max_n)) {
        int mpos = pattern_find(s->data, (int)s->len, p->data, (int)p->len, pos);
        if (mpos < 0) break;
        int mlen = pattern_match_at(p->data, (int)p->len, s->data, mpos, (int)s->len);
        if (mlen < 0) break;
        /* Copy the part before the match. */
        while (len + (mpos - pos) + 1 >= cap) { cap *= 2; buf = lv_realloc(buf, cap); }
        memcpy(buf + len, s->data + pos, mpos - pos);
        len += mpos - pos;
        /* Compute replacement. */
        if (lv_is_string(repl)) {
            lv_string_t *rs = (lv_string_t *)vtx_heap_ptr(repl);
            while (len + rs->len + 1 >= cap) { cap *= 2; buf = lv_realloc(buf, cap); }
            memcpy(buf + len, rs->data, rs->len);
            len += rs->len;
        } else {
            /* For non-string replacements, use tostring. */
            size_t rl;
            char *rs = lv_to_string(repl, &rl);
            while (len + rl + 1 >= cap) { cap *= 2; buf = lv_realloc(buf, cap); }
            memcpy(buf + len, rs, rl);
            len += rl;
            lv_free(rs);
        }
        count++;
        pos = mpos + (mlen > 0 ? mlen : 1);  /* advance past match, at least 1 */
    }
    /* Copy the remainder. */
    while (len + (s->len - pos) + 1 >= cap) { cap *= 2; buf = lv_realloc(buf, cap); }
    memcpy(buf + len, s->data + pos, s->len - pos);
    len += s->len - pos;
    buf[len] = 0;
    vtx_value_t result = make_str(rt, buf, len);
    lv_free(buf);
    /* gsub returns (new_string, count). For MVP, return just the string. */
    return result;
}

/* ---- Expanded math library ---- */
static vtx_value_t fn_math_atan2(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 2) return VTX_VALUE_NULL;
    return vtx_make_double(atan2(to_d(argv[0]), to_d(argv[1])));
}
static vtx_value_t fn_math_asin(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    return math_unary(rt, asin, argc, argv);
}
static vtx_value_t fn_math_acos(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    return math_unary(rt, acos, argc, argv);
}
static vtx_value_t fn_math_tanh(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    return math_unary(rt, tanh, argc, argv);
}
static vtx_value_t fn_math_sinh(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    return math_unary(rt, sinh, argc, argv);
}
static vtx_value_t fn_math_cosh(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    return math_unary(rt, cosh, argc, argv);
}
static vtx_value_t fn_math_fmod(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 2) return VTX_VALUE_NULL;
    return vtx_make_double(fmod(to_d(argv[0]), to_d(argv[1])));
}
static vtx_value_t fn_math_type(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1) return make_cstr(rt, "nil");
    if (vtx_is_smi(argv[0])) return make_cstr(rt, "integer");
    if (vtx_is_double(argv[0])) return make_cstr(rt, "float");
    return VTX_VALUE_NULL;
}
static vtx_value_t fn_math_tointeger(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1) return VTX_VALUE_NULL;
    int64_t i;
    if (lv_to_int(argv[0], &i)) return vtx_make_smi(i);
    return VTX_VALUE_NULL;
}

/* ---- Expanded table library ---- */
static vtx_value_t fn_tbl_move(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 4 || !lv_is_table(argv[0])) lv_error(rt, "bad argument to table.move");
    lv_table_t *a1 = (lv_table_t *)vtx_heap_ptr(argv[0]);
    int64_t f = vtx_smi_value(argv[1]);
    int64_t e = vtx_smi_value(argv[2]);
    int64_t t = vtx_smi_value(argv[3]);
    lv_table_t *a2 = (argc >= 5 && lv_is_table(argv[4])) ? (lv_table_t *)vtx_heap_ptr(argv[4]) : a1;
    for (int64_t i = 0; i <= e - f; i++) {
        vtx_value_t v = lv_table_get(a1, vtx_make_smi(f + i));
        lv_table_set(a2, vtx_make_smi(t + i), v);
    }
    return lv_make_table_val(a2);
}

static vtx_value_t fn_tbl_pack(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    lv_table_t *t = lv_table_new(argc + 1);
    for (int i = 0; i < argc; i++) {
        lv_table_set(t, vtx_make_smi(i + 1), argv[i]);
    }
    lv_table_set(t, lv_runtime_intern_string(rt, "n", 1), vtx_make_smi(argc));
    return lv_make_table_val(t);
}

/* ---- Expanded os library ---- */
static vtx_value_t fn_os_getenv(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_string(argv[0])) return VTX_VALUE_NULL;
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    char *name = lv_strndup(s->data, s->len);
    const char *val = getenv(name);
    lv_free(name);
    if (!val) return VTX_VALUE_NULL;
    return make_cstr(rt, val);
}

static vtx_value_t fn_os_execute(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1 || !lv_is_string(argv[0])) return vtx_make_smi(0);
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    char *cmd = lv_strndup(s->data, s->len);
    int rc = system(cmd);
    lv_free(cmd);
    return vtx_make_smi(rc);
}

static vtx_value_t fn_os_exit(int argc, vtx_value_t *argv, void *ud) {
    int code = (argc >= 1 && vtx_is_smi(argv[0])) ? (int)vtx_smi_value(argv[0]) : 0;
    exit(code);
    return VTX_VALUE_NULL;
}

/* ---- Expanded io library (basic file I/O) ----
 * File objects are represented as lv_function_t... no, actually we use
 * a simple wrapper. For MVP, we store FILE* in a heap-allocated struct. */
typedef struct {
    lv_object_t hdr;
    FILE *fp;
    bool owned;
} lv_file_t;

static lv_kind_t LV_KIND_FILE = 5;

static vtx_value_t make_file_val(lv_runtime_t *rt, FILE *fp, bool owned) {
    lv_file_t *f = lv_alloc(sizeof(*f));
    f->hdr.kind = LV_KIND_FILE;
    f->hdr.gc_mark = 0;
    f->fp = fp;
    f->owned = owned;
    return vtx_make_heap_ptr(f);
}

static lv_file_t *lv_value_file(vtx_value_t v) {
    if (lv_value_kind(v) == LV_KIND_FILE) return (lv_file_t *)vtx_heap_ptr(v);
    return NULL;
}

static vtx_value_t fn_io_open(int argc, vtx_value_t *argv, void *ud) {
    lv_runtime_t *rt = GET_RT(ud);
    if (!rt) rt = lv_eval_get_runtime();
    if (argc < 1 || !lv_is_string(argv[0])) return VTX_VALUE_NULL;
    lv_string_t *s = (lv_string_t *)vtx_heap_ptr(argv[0]);
    char *path = lv_strndup(s->data, s->len);
    const char *mode = "r";
    char mode_buf[8];
    if (argc >= 2 && lv_is_string(argv[1])) {
        lv_string_t *ms = (lv_string_t *)vtx_heap_ptr(argv[1]);
        size_t n = ms->len < 7 ? ms->len : 7;
        memcpy(mode_buf, ms->data, n);
        mode_buf[n] = 0;
        mode = mode_buf;
    }
    FILE *fp = fopen(path, mode);
    lv_free(path);
    if (!fp) return VTX_VALUE_NULL;
    return make_file_val(rt, fp, true);
}

static vtx_value_t fn_io_close(int argc, vtx_value_t *argv, void *ud) {
    if (argc < 1) return VTX_VALUE_FALSE;
    lv_file_t *f = lv_value_file(argv[0]);
    if (!f || !f->fp) return VTX_VALUE_FALSE;
    if (f->owned) fclose(f->fp);
    f->fp = NULL;
    return VTX_VALUE_TRUE;
}

static vtx_value_t fn_io_lines(int argc, vtx_value_t *argv, void *ud) {
    /* For MVP, return nil. Lines iterators need coroutine support. */
    (void)argc; (void)argv;
    return VTX_VALUE_NULL;
}

/* Also add file:read and file:write as method dispatch. The method
 * dispatch for file objects checks the kind and routes to the right
 * function. This is handled in lv_fn_call_method. */

/* ---- Scope table helpers (for compiled function bodies) ----
 * Function bodies use a "scope table" model: local 0 is a Lua table
 * representing the current invocation's scope. The table has a special
 * key "__parent" pointing to the enclosing scope (the closure's
 * captured env). Variable lookups walk the parent chain; assignments
 * update the first scope that has the variable, or fall back to the
 * global env.
 *
 * This makes closures work correctly: each closure captures its
 * defining scope table BY REFERENCE, so mutations to captured
 * variables are visible across invocations of the same closure. */

/* Special key for the parent scope pointer. Stored as an interned string. */
static vtx_value_t scope_parent_key(lv_runtime_t *rt) {
    return lv_runtime_intern_string(rt, "__parent", 8);
}

/* LV_FN_SCOPE_GET = 240: (scope, name) → value
 * Walks the scope chain looking for `name`. Falls back to global env. */
static vtx_value_t lv_fn_scope_get(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    if (argc < 2 || !lv_is_table(argv[0])) {
        return VTX_VALUE_NULL;
    }
    vtx_value_t key = argv[1];
    vtx_value_t parent_k = scope_parent_key(rt);
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(argv[0]);
    /* Walk the chain. */
    for (int depth = 0; depth < 256; depth++) {
        vtx_value_t v = lv_table_get(t, key);
        if (!lv_is_nil(v)) return v;
        /* Check __parent. */
        vtx_value_t parent = lv_table_get(t, parent_k);
        if (!lv_is_table(parent)) break;
        t = (lv_table_t *)vtx_heap_ptr(parent);
    }
    /* Not found in scope chain — check global env. */
    if (rt->current_env) {
        return lv_table_get(rt->current_env, key);
    }
    return VTX_VALUE_NULL;
}

/* LV_FN_SCOPE_SET = 241: (scope, name, value) → nil
 * Walks the scope chain. If `name` exists in some scope, updates it
 * there. Otherwise, sets it in the global env (global assignment). */
static vtx_value_t lv_fn_scope_set(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    if (argc < 3 || !lv_is_table(argv[0])) {
        return VTX_VALUE_NULL;
    }
    vtx_value_t scope = argv[0];
    vtx_value_t key = argv[1];
    vtx_value_t val = argv[2];
    vtx_value_t parent_k = scope_parent_key(rt);
    lv_table_t *t = (lv_table_t *)vtx_heap_ptr(scope);
    for (int depth = 0; depth < 256; depth++) {
        /* Check if this scope has the key. */
        lv_table_entry_t *e = lv_table_find_slot(t, key);
        if (e && e->key != VTX_VALUE_UNDEFINED) {
            /* Found — update in place. */
            lv_table_set(t, key, val);
            return VTX_VALUE_NULL;
        }
        /* Check __parent. */
        vtx_value_t parent = lv_table_get(t, parent_k);
        if (!lv_is_table(parent)) break;
        t = (lv_table_t *)vtx_heap_ptr(parent);
    }
    /* Not found in any scope — set in global env. */
    if (rt->current_env) {
        lv_table_set(rt->current_env, key, val);
    }
    return VTX_VALUE_NULL;
}

/* LV_FN_SCOPE_DECLARE = 242: (scope, name, value) → nil
 * Declares a new local in the innermost scope (always sets on argv[0],
 * doesn't walk the chain). Used for `local x = ...` in function bodies. */
static vtx_value_t lv_fn_scope_declare(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    if (argc < 3 || !lv_is_table(argv[0])) return VTX_VALUE_NULL;
    lv_table_set((lv_table_t *)vtx_heap_ptr(argv[0]), argv[1], argv[2]);
    return VTX_VALUE_NULL;
}

/* LV_FN_NEW_SCOPE = 243: (parent) → new_scope
 * Creates a new scope table with __parent set to `parent`. */
static vtx_value_t lv_fn_new_scope(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    lv_table_t *t = lv_table_new(8);
    if (argc >= 1 && lv_is_table(argv[0])) {
        lv_table_set(t, scope_parent_key(rt), argv[0]);
    } else {
        /* No parent — link to global env. */
        lv_table_set(t, scope_parent_key(rt), lv_make_table_val(rt->current_env));
    }
    return lv_make_table_val(t);
}

/* LV_FN_NEW_CLOSURE_WITH_ENV = 244: (proto_id, captured_env) → closure
 * Creates a Lua closure that carries a captured scope table. When
 * called, the runtime creates a new child scope of captured_env,
 * populates it with parameters, and runs the function's compiled
 * bytecode. */
static vtx_value_t lv_fn_new_closure_with_env(lv_runtime_t *rt, vtx_value_t *argv, int argc) {
    if (argc < 2 || !vtx_is_smi(argv[0])) return VTX_VALUE_NULL;
    int proto_id = (int)vtx_smi_value(argv[0]);
    vtx_value_t env = argv[1];
    if (!lv_is_table(env)) env = lv_make_table_val(rt->current_env);
    /* Look up the compiled bytecode for this proto. */
    vtx_bytecode_t *bc = lv_runtime_get_proto_bytecode(rt, proto_id);
    if (!bc) {
        lv_error(rt, "internal: no compiled bytecode for proto_id %d", proto_id);
        return VTX_VALUE_NULL;
    }
    lv_function_t *f = lv_function_new_lua(proto_id);
    f->rt = rt;
    f->enclosing_scope = NULL;  /* not used for compiled closures */
    f->captured_env = vtx_heap_ptr(env);  /* store the captured env table */
    f->compiled_bc = bc;  /* cache the bytecode pointer */
    return lv_make_function_val(f);
}

/* ---- Dispatch table ----
 * Maps lua_fn_id → C function. Used by both the bytecode CALL_RUNTIME
 * path and the evaluator's arithmetic/comparison helpers. */
vtx_value_t lv_stdlib_dispatch(lv_runtime_t *rt, uint16_t lua_fn_id,
                                vtx_value_t *argv, int arg_count) {
    switch (lua_fn_id) {
    /* Basic functions */
    case 100: return fn_print(arg_count, argv, rt);
    case 101: return fn_tostring(arg_count, argv, rt);
    case 102: return fn_tonumber(arg_count, argv, rt);
    case 103: return fn_type(arg_count, argv, rt);
    case 104: return fn_assert(arg_count, argv, rt);
    case 105: return fn_error(arg_count, argv, rt);
    case 106: return fn_pcall(arg_count, argv, rt);
    case 107: return fn_select(arg_count, argv, rt);
    case 108: return fn_rawget(arg_count, argv, rt);
    case 109: return fn_rawset(arg_count, argv, rt);
    case 110: return fn_rawequal(arg_count, argv, rt);
    case 111: return fn_rawlen(arg_count, argv, rt);
    case 112: return fn_next(arg_count, argv, rt);
    case 113: return fn_pairs(arg_count, argv, rt);
    case 114: return fn_ipairs(arg_count, argv, rt);
    case 115: return fn_unpack(arg_count, argv, rt);
    case 116: return fn_setmetatable(arg_count, argv, rt);
    case 117: return fn_getmetatable(arg_count, argv, rt);
    case 118: return vtx_make_bool(lv_is_truthy(arg_count >= 1 ? argv[0] : VTX_VALUE_NULL));
    /* Arithmetic */
    case 120: return arith_add(rt, argv[0], argv[1]);
    case 121: return arith_sub(rt, argv[0], argv[1]);
    case 122: return arith_mul(rt, argv[0], argv[1]);
    case 123: return arith_div(rt, argv[0], argv[1]);
    case 124: return arith_idiv(rt, argv[0], argv[1]);
    case 125: return arith_mod(rt, argv[0], argv[1]);
    case 126: return arith_pow(rt, argv[0], argv[1]);
    case 127: return arith_concat(rt, argv[0], argv[1]);
    /* Comparisons */
    case 130: return cmp_eq(rt, argv[0], argv[1]);
    case 131: return cmp_lt(rt, argv[0], argv[1]);
    case 132: return cmp_le(rt, argv[0], argv[1]);
    /* Bitwise */
    case 140: return bit_and(rt, argv[0], argv[1]);
    case 141: return bit_or(rt, argv[0], argv[1]);
    case 142: return bit_xor(rt, argv[0], argv[1]);
    case 143: return bit_shl(rt, argv[0], argv[1]);
    case 144: return bit_shr(rt, argv[0], argv[1]);
    /* String operations */
    case 150: return fn_str_len(arg_count, argv, rt);
    case 151: return fn_str_sub(arg_count, argv, rt);
    case 152: return fn_str_upper(arg_count, argv, rt);
    case 153: return fn_str_lower(arg_count, argv, rt);
    case 154: return fn_str_rep(arg_count, argv, rt);
    case 155: return fn_str_reverse(arg_count, argv, rt);
    case 156: return fn_str_byte(arg_count, argv, rt);
    case 157: return fn_str_char(arg_count, argv, rt);
    case 158: return fn_str_format(arg_count, argv, rt);
    case 159: return fn_str_find(arg_count, argv, rt);
    /* Table operations */
    case 170: return fn_tbl_insert(arg_count, argv, rt);
    case 171: return fn_tbl_remove(arg_count, argv, rt);
    case 172: return fn_tbl_concat(arg_count, argv, rt);
    case 173: return fn_tbl_sort(arg_count, argv, rt);
    case 174: return fn_unpack(arg_count, argv, rt);
    /* Math */
    case 180: return fn_math_abs(arg_count, argv, rt);
    case 181: return fn_math_floor(arg_count, argv, rt);
    case 182: return fn_math_ceil(arg_count, argv, rt);
    case 183: return fn_math_sqrt(arg_count, argv, rt);
    case 184: return fn_math_sin(arg_count, argv, rt);
    case 185: return fn_math_cos(arg_count, argv, rt);
    case 186: return fn_math_tan(arg_count, argv, rt);
    case 187: return fn_math_log(arg_count, argv, rt);
    case 188: return fn_math_exp(arg_count, argv, rt);
    case 189: return fn_math_pow(arg_count, argv, rt);
    case 190: return fn_math_max(arg_count, argv, rt);
    case 191: return fn_math_min(arg_count, argv, rt);
    case 192: return fn_math_random(arg_count, argv, rt);
    case 193: return vtx_make_double(3.14159265358979323846);
    case 194: return vtx_make_double(INFINITY);
    /* I/O */
    case 200: return fn_io_write(arg_count, argv, rt);
    case 201: return fn_io_read(arg_count, (vtx_value_t*)argv, rt);
    /* OS */
    case 210: return fn_os_time(arg_count, argv, rt);
    case 211: return fn_os_clock(arg_count, argv, rt);
    case 212: return fn_os_date(arg_count, argv, rt);
    /* Function call */
    case 220: return lv_fn_call(rt, argv, arg_count);
    case 221: return lv_fn_call_method(rt, argv, arg_count);
    case 222: return lv_fn_new_table(rt, argv, arg_count);
    case 223: return lv_fn_set_field(rt, argv, arg_count);
    case 224: return lv_fn_get_field(rt, argv, arg_count);
    case 225: return lv_fn_new_closure(rt, argv, arg_count);
    case 226: return lv_fn_vararg(rt, argv, arg_count);
    case 227: return lv_length(rt, argv[0]);
    case 228: return lv_fn_global_get(rt, argv, arg_count);
    case 229: return lv_fn_global_set(rt, argv, arg_count);
    /* Scope table helpers (for compiled function bodies) */
    case 240: return lv_fn_scope_get(rt, argv, arg_count);
    case 241: return lv_fn_scope_set(rt, argv, arg_count);
    case 242: return lv_fn_scope_declare(rt, argv, arg_count);
    case 243: return lv_fn_new_scope(rt, argv, arg_count);
    case 244: return lv_fn_new_closure_with_env(rt, argv, arg_count);
    /* Expanded string library */
    case 250: return fn_str_gmatch(arg_count, argv, rt);
    case 251: return fn_str_gsub(arg_count, argv, rt);
    case 252: return fn_str_match(arg_count, argv, rt);
    /* Expanded math library */
    case 260: return fn_math_atan2(arg_count, argv, rt);
    case 261: return fn_math_asin(arg_count, argv, rt);
    case 262: return fn_math_acos(arg_count, argv, rt);
    case 263: return fn_math_tanh(arg_count, argv, rt);
    case 264: return fn_math_sinh(arg_count, argv, rt);
    case 265: return fn_math_cosh(arg_count, argv, rt);
    case 266: return fn_math_fmod(arg_count, argv, rt);
    case 267: return fn_math_type(arg_count, argv, rt);
    case 268: return fn_math_tointeger(arg_count, argv, rt);
    /* Expanded table library */
    case 270: return fn_tbl_move(arg_count, argv, rt);
    case 271: return fn_tbl_pack(arg_count, argv, rt);
    /* Expanded os library */
    case 280: return fn_os_getenv(arg_count, argv, rt);
    case 281: return fn_os_execute(arg_count, argv, rt);
    case 282: return fn_os_exit(arg_count, argv, rt);
    /* Expanded io library */
    case 290: return fn_io_open(arg_count, argv, rt);
    case 291: return fn_io_close(arg_count, argv, rt);
    case 292: return fn_io_lines(arg_count, argv, rt);
    }
    lv_error(rt, "internal: unknown lua_fn_id %u", lua_fn_id);
    return VTX_VALUE_NULL;
}

/* ---- String library table ---- */
static lv_table_t *g_string_lib = NULL;

lv_table_t *lv_stdlib_string_lib(lv_runtime_t *rt) {
    return g_string_lib;
}

/* ---- Registration ---- */
static void register_subtable(lv_runtime_t *rt, const char *name, lv_table_t *t) {
    lv_table_set(rt->globals, make_cstr(rt, name), lv_make_table_val(t));
}

static void register_native_in(lv_runtime_t *rt, lv_table_t *t, const char *name, lv_native_fn fn) {
    lv_function_t *f = lv_function_new_native(
        (vtx_value_t (*)(int, vtx_value_t *, void *))fn, rt);
    lv_table_set(t, make_cstr(rt, name), lv_make_function_val(f));
}

void lv_stdlib_register(lv_runtime_t *rt) {
    /* Basic functions. */
    register_native(rt, "print",     fn_print);
    register_native(rt, "tostring",  fn_tostring);
    register_native(rt, "tonumber",  fn_tonumber);
    register_native(rt, "type",      fn_type);
    register_native(rt, "assert",    fn_assert);
    register_native(rt, "error",     fn_error);
    register_native(rt, "pcall",     fn_pcall);
    register_native(rt, "select",    fn_select);
    register_native(rt, "rawget",    fn_rawget);
    register_native(rt, "rawset",    fn_rawset);
    register_native(rt, "rawequal",  fn_rawequal);
    register_native(rt, "rawlen",    fn_rawlen);
    register_native(rt, "next",      fn_next);
    register_native(rt, "pairs",     fn_pairs);
    register_native(rt, "ipairs",    fn_ipairs);
    register_native(rt, "unpack",    fn_unpack);
    register_native(rt, "setmetatable", fn_setmetatable);
    register_native(rt, "getmetatable", fn_getmetatable);

    /* Math library. */
    lv_table_t *math = lv_table_new(40);
    register_subtable(rt, "math", math);
    register_native_in(rt, math, "abs",       fn_math_abs);
    register_native_in(rt, math, "floor",     fn_math_floor);
    register_native_in(rt, math, "ceil",      fn_math_ceil);
    register_native_in(rt, math, "sqrt",      fn_math_sqrt);
    register_native_in(rt, math, "sin",       fn_math_sin);
    register_native_in(rt, math, "cos",       fn_math_cos);
    register_native_in(rt, math, "tan",       fn_math_tan);
    register_native_in(rt, math, "log",       fn_math_log);
    register_native_in(rt, math, "exp",       fn_math_exp);
    register_native_in(rt, math, "pow",       fn_math_pow);
    register_native_in(rt, math, "max",       fn_math_max);
    register_native_in(rt, math, "min",       fn_math_min);
    register_native_in(rt, math, "random",    fn_math_random);
    register_native_in(rt, math, "atan",      fn_math_atan2);  /* atan2 via atan */
    register_native_in(rt, math, "asin",      fn_math_asin);
    register_native_in(rt, math, "acos",      fn_math_acos);
    register_native_in(rt, math, "tanh",      fn_math_tanh);
    register_native_in(rt, math, "sinh",      fn_math_sinh);
    register_native_in(rt, math, "cosh",      fn_math_cosh);
    register_native_in(rt, math, "fmod",      fn_math_fmod);
    register_native_in(rt, math, "type",      fn_math_type);
    register_native_in(rt, math, "tointeger", fn_math_tointeger);
    lv_table_set(math, make_cstr(rt, "pi"),    vtx_make_double(3.14159265358979323846));
    lv_table_set(math, make_cstr(rt, "huge"),  vtx_make_double(INFINITY));
    lv_table_set(math, make_cstr(rt, "maxinteger"), vtx_make_double((double)INT64_MAX));
    lv_table_set(math, make_cstr(rt, "mininteger"), vtx_make_double((double)INT64_MIN));

    /* String library. */
    lv_table_t *strlib = lv_table_new(32);
    g_string_lib = strlib;
    register_subtable(rt, "string", strlib);
    register_native_in(rt, strlib, "len",     fn_str_len);
    register_native_in(rt, strlib, "sub",     fn_str_sub);
    register_native_in(rt, strlib, "upper",   fn_str_upper);
    register_native_in(rt, strlib, "lower",   fn_str_lower);
    register_native_in(rt, strlib, "rep",     fn_str_rep);
    register_native_in(rt, strlib, "reverse", fn_str_reverse);
    register_native_in(rt, strlib, "byte",    fn_str_byte);
    register_native_in(rt, strlib, "char",    fn_str_char);
    register_native_in(rt, strlib, "format",  fn_str_format);
    register_native_in(rt, strlib, "find",    fn_str_find);
    register_native_in(rt, strlib, "match",   fn_str_match);
    register_native_in(rt, strlib, "gmatch",  fn_str_gmatch);
    register_native_in(rt, strlib, "gsub",    fn_str_gsub);

    /* Table library. */
    lv_table_t *tablib = lv_table_new(16);
    register_subtable(rt, "table", tablib);
    register_native_in(rt, tablib, "insert",  fn_tbl_insert);
    register_native_in(rt, tablib, "remove",  fn_tbl_remove);
    register_native_in(rt, tablib, "concat",  fn_tbl_concat);
    register_native_in(rt, tablib, "sort",    fn_tbl_sort);
    register_native_in(rt, tablib, "unpack",  fn_unpack);
    register_native_in(rt, tablib, "move",    fn_tbl_move);
    register_native_in(rt, tablib, "pack",    fn_tbl_pack);

    /* I/O library. */
    lv_table_t *iolib = lv_table_new(8);
    register_subtable(rt, "io", iolib);
    register_native_in(rt, iolib, "write",    fn_io_write);
    register_native_in(rt, iolib, "read",     (lv_native_fn)fn_io_read);
    register_native_in(rt, iolib, "open",     fn_io_open);
    register_native_in(rt, iolib, "close",    fn_io_close);
    register_native_in(rt, iolib, "lines",    fn_io_lines);

    /* OS library. */
    lv_table_t *oslib = lv_table_new(16);
    register_subtable(rt, "os", oslib);
    register_native_in(rt, oslib, "time",     fn_os_time);
    register_native_in(rt, oslib, "clock",    fn_os_clock);
    register_native_in(rt, oslib, "date",     fn_os_date);
    register_native_in(rt, oslib, "getenv",   fn_os_getenv);
    register_native_in(rt, oslib, "execute",  fn_os_execute);
    register_native_in(rt, oslib, "exit",     fn_os_exit);

    /* _VERSION */
    lv_table_set(rt->globals, make_cstr(rt, "_VERSION"),
                 make_cstr(rt, "LuaVortex 0.1"));
}
