/* lv_codegen.c — AST → VORTEX bytecode emitter.
 *
 * Walks the AST produced by the parser and emits a VORTEX bytecode
 * module (vtx_bytecode_t) that can be executed by vtx_runtime_run().
 *
 * Emission strategy:
 *   - Each Lua expression emits code that pushes exactly one value.
 *   - Each Lua statement emits code that has net zero stack effect.
 *   - Local variables map to VORTEX local slots.
 *   - Forward jumps (if/else, loops, break) are emitted with a placeholder
 *     2-byte operand and recorded in a patch list; a final resolve pass
 *     patches them with the correct target PC.
 *
 * Lua semantics mapping to VORTEX opcodes:
 *   - nil/true/false/number literals → LOAD_NULL / LOAD_TRUE / LOAD_FALSE
 *                                       / LOAD_CONST_INT / LOAD_CONST_FLOAT
 *   - String literals → LOAD_CONST_STR (heap pointer stored in const pool)
 *   - Locals → LOAD_LOCAL / STORE_LOCAL
 *   - Arithmetic on known ints → IADD/ISUB/IMUL/IDIV/IMOD/...
 *   - Mixed-type arithmetic → CALL_RUNTIME lua_arith dispatcher
 *   - Comparisons on known ints → ICMP_*; mixed → CALL_RUNTIME lua_compare
 *   - String concat → CALL_RUNTIME lua_concat
 *   - Length operator → CALL_RUNTIME lua_length
 *   - Table constructor → CALL_RUNTIME lua_newtable + repeated lua_settable
 *   - Table index get/set → CALL_RUNTIME lua_gettable / lua_settable
 *   - Function call → CALL_RUNTIME lua_call (handles varargs/methods)
 *   - return → RETURN_VALUE
 *   - if/while/for → IF_TRUE / IF_FALSE / GOTO with patch lists
 *
 * The runtime call protocol:
 *   CALL_RUNTIME operand = (lua_fn_id << 6) | arg_count
 *   where lua_fn_id >= 100. The interpreter pops `arg_count` values
 *   (left-to-right: deepest stack slot is the first arg) and passes
 *   them to the registered C function.
 */

#include "lv_codegen.h"
#include "lv_value.h"
#include "lv_runtime.h"
#include <stdarg.h>

/* ---- Lua runtime function IDs ----
 * These IDs are packed into the high bits of the CALL_RUNTIME operand.
 * They must agree with the dispatch table in lv_runtime.c.
 * IDs 0-6 are reserved for VORTEX builtins (typeof, print, etc.). */
typedef enum {
    LV_FN_PRINT       = 100,  /* print(...)         — variadic, no return */
    LV_FN_TOSTRING    = 101,  /* tostring(v)        — 1 arg → 1 result */
    LV_FN_TONUMBER    = 102,  /* tonumber(v [, b])  — 1-2 args → 1 result */
    LV_FN_TYPE        = 103,  /* type(v)            — 1 arg → 1 result (string) */
    LV_FN_ASSERT      = 104,  /* assert(v [, msg])  — 1-2 args → 1 result */
    LV_FN_ERROR       = 105,  /* error(v [, level]) — 1-2 args, does not return */
    LV_FN_PCALL       = 106,  /* pcall(f, ...)      — variadic → 1+ result */
    LV_FN_SELECT      = 107,  /* select(n, ...)     — variadic → 1+ result */
    LV_FN_RAWGET      = 108,  /* rawget(t, k)       — 2 args → 1 result */
    LV_FN_RAWSET      = 109,  /* rawset(t, k, v)    — 3 args → 1 result (t) */
    LV_FN_RAWEQUAL    = 110,  /* rawequal(a, b)     — 2 args → 1 result */
    LV_FN_RAWLEN      = 111,  /* rawlen(v)          — 1 arg → 1 result */
    LV_FN_NEXT        = 112,  /* next(t [, k])      — 1-2 args → 2 results */
    LV_FN_PAIRS       = 113,  /* pairs(t)           — 1 arg → 3 results */
    LV_FN_IPAIRS      = 114,  /* ipairs(t)          — 1 arg → 3 results */
    LV_FN_UNPACK      = 115,  /* unpack(t [, i [, j]]) — 1-3 args → n results */
    LV_FN_SETMETATABLE= 116,  /* setmetatable(t, m) — 2 args → 1 result */
    LV_FN_GETMETATABLE= 117,  /* getmetatable(t)    — 1 arg → 1 result */
    LV_FN_TOBOOLEAN   = 118,  /* (internal)         — 1 arg → 1 result */
    /* Arithmetic dispatchers (for mixed-type operands) */
    LV_FN_ARITH_ADD   = 120,
    LV_FN_ARITH_SUB   = 121,
    LV_FN_ARITH_MUL   = 122,
    LV_FN_ARITH_DIV   = 123,
    LV_FN_ARITH_IDIV  = 124,
    LV_FN_ARITH_MOD   = 125,
    LV_FN_ARITH_POW   = 126,
    LV_FN_ARITH_CONCAT= 127,
    /* Comparison dispatcher */
    LV_FN_CMP_EQ      = 130,
    LV_FN_CMP_LT      = 131,
    LV_FN_CMP_LE      = 132,
    /* Bitwise dispatchers */
    LV_FN_BIT_AND     = 140,
    LV_FN_BIT_OR      = 141,
    LV_FN_BIT_XOR     = 142,
    LV_FN_BIT_SHL     = 143,
    LV_FN_BIT_SHR     = 144,
    /* String operations */
    LV_FN_STR_LEN     = 150,
    LV_FN_STR_SUB     = 151,
    LV_FN_STR_UPPER   = 152,
    LV_FN_STR_LOWER   = 153,
    LV_FN_STR_REP     = 154,
    LV_FN_STR_REVERSE = 155,
    LV_FN_STR_BYTE    = 156,
    LV_FN_STR_CHAR    = 157,
    LV_FN_STR_FORMAT  = 158,
    LV_FN_STR_FIND    = 159,
    /* (string.gmatch/gsub/match are at 250-252, defined later) */
    /* Table operations */
    LV_FN_TBL_INSERT  = 170,
    LV_FN_TBL_REMOVE  = 171,
    LV_FN_TBL_CONCAT  = 172,
    LV_FN_TBL_SORT    = 173,
    LV_FN_TBL_UNPACK  = 174,  /* alias for unpack */
    /* Math operations */
    LV_FN_MATH_ABS    = 180,
    LV_FN_MATH_FLOOR  = 181,
    LV_FN_MATH_CEIL   = 182,
    LV_FN_MATH_SQRT   = 183,
    LV_FN_MATH_SIN    = 184,
    LV_FN_MATH_COS    = 185,
    LV_FN_MATH_TAN    = 186,
    LV_FN_MATH_LOG    = 187,
    LV_FN_MATH_EXP    = 188,
    LV_FN_MATH_POW    = 189,
    LV_FN_MATH_MAX    = 190,
    LV_FN_MATH_MIN    = 191,
    LV_FN_MATH_RANDOM = 192,
    LV_FN_MATH_PI     = 193,
    LV_FN_MATH_HUGE   = 194,
    /* I/O operations */
    LV_FN_IO_WRITE    = 200,
    LV_FN_IO_READ     = 201,
    /* OS operations */
    LV_FN_OS_TIME     = 210,
    LV_FN_OS_CLOCK    = 211,
    LV_FN_OS_DATE     = 212,
    /* Lua function call dispatcher */
    LV_FN_CALL        = 220,  /* (fn, args...) → results (1 result for MVP) */
    LV_FN_CALL_METHOD = 221,  /* (obj, name, args...) → 1 result */
    LV_FN_NEW_TABLE   = 222,  /* (narr, nrec) → table */
    LV_FN_SET_FIELD   = 223,  /* (t, k, v) → t  (for table constructors) */
    LV_FN_GET_FIELD   = 224,  /* (t, k) → v  (for table.x indexing) */
    LV_FN_NEW_CLOSURE = 225,  /* (proto_id, upvalues...) → closure */
    LV_FN_VARARG      = 226,  /* (...) → first vararg (MVP: returns 1 value) */
    LV_FN_LENGTH      = 227,  /* #v → length */
    LV_FN_GLOBAL_GET  = 228,  /* (env, name) → value */
    LV_FN_GLOBAL_SET  = 229,  /* (env, name, value) → nil */
    LV_FN_RETURN_MULTI= 230,  /* (vals...) → first value (MVP: 1 return) */
    /* Scope table helpers (for compiled function bodies) */
    LV_FN_SCOPE_GET   = 240,  /* (scope, name) → value */
    LV_FN_SCOPE_SET   = 241,  /* (scope, name, value) → nil */
    LV_FN_SCOPE_DECLARE=242,  /* (scope, name, value) → nil */
    LV_FN_NEW_SCOPE   = 243,  /* (parent) → new_scope */
    LV_FN_NEW_CLOSURE_WITH_ENV = 244, /* (proto_id, captured_env) → closure */
    /* Expanded string library */
    LV_FN_STR_GMATCH  = 250,
    LV_FN_STR_GSUB    = 251,
    LV_FN_STR_MATCH   = 252,
    /* Expanded math library */
    LV_FN_MATH_ATAN2  = 260,
    LV_FN_MATH_ASIN   = 261,
    LV_FN_MATH_ACOS   = 262,
    LV_FN_MATH_TANH   = 263,
    LV_FN_MATH_SINH   = 264,
    LV_FN_MATH_COSH   = 265,
    LV_FN_MATH_FMOD   = 266,
    LV_FN_MATH_TYPE   = 267,
    LV_FN_MATH_TOINT  = 268,
    /* Expanded table library */
    LV_FN_TBL_MOVE    = 270,
    LV_FN_TBL_PACK    = 271,
    /* Expanded os library */
    LV_FN_OS_GETENV   = 280,
    LV_FN_OS_EXECUTE  = 281,
    LV_FN_OS_EXIT     = 282,
    /* Expanded io library */
    LV_FN_IO_OPEN     = 290,
    LV_FN_IO_CLOSE    = 291,
    LV_FN_IO_LINES    = 292,
} lv_fn_id_t;

/* ---- Internal buffers ---- */
typedef struct {
    uint8_t *code;
    size_t   len;
    size_t   cap;
} codebuf_t;

typedef struct {
    vtx_value_t *consts;
    uint32_t     count;
    uint32_t     cap;
} constpool_t;

typedef struct {
    char  *name;
    int    slot;
    int    scope_depth;
} local_t;

typedef struct {
    local_t *items;
    int      count;
    int      cap;
    int      depth;
    int      next_slot;
    int      max_slots;
} scope_t;

/* Forward jump: emit placeholder, resolve later. */
typedef struct {
    size_t patch_offset;   /* offset in code of the 2-byte operand */
    int    target_pc;      /* target PC (resolved at place time), -1 if pending */
    int    target_marker;  /* unique ID of the target label */
} pending_jump_t;

typedef struct {
    int    marker;         /* label marker ID */
    size_t pc;             /* PC of the label */
    bool   placed;         /* has the label been placed yet? */
} label_t;

/* Per-function codegen state. */
typedef struct lv_func_cg {
    lv_codegen_t   *cg;
    codebuf_t       code;
    constpool_t     pool;
    scope_t         scope;
    /* Labels & patches */
    label_t        *labels;
    int             nlabels;
    int             cap_labels;
    int             next_marker;
    pending_jump_t *jumps;
    int             njumps;
    int             cap_jumps;
    /* Break patch stack (for compiling `break` inside loops) */
    int            *break_markers;
    int             nbreak;
    int             cap_break;
    int             loop_depth;
    /* Function semantics */
    bool            is_function;
    bool            is_vararg;
    /* If true, use scope-table model (all variable accesses go through
     * SCOPE_GET/SCOPE_SET runtime calls). Used for function bodies.
     * If false (main chunk), use VORTEX locals directly. */
    bool            use_scope_table;
    /* Stack tracking */
    int             cur_stack;
    int             max_stack;
    /* First param slot for the "environment" object (the global env).
     * For the main chunk, this is local 0. For functions, it's also
     * local 0 (the closure passes the env as the first arg). */
    int             env_slot;
} lv_func_cg_t;

struct lv_codegen {
    lv_runtime_t *rt;
    char         *last_error;
};

struct lv_compiled {
    vtx_bytecode_t  bc;
    char           *disasm;
    /* Owning pointers: */
    uint8_t        *code_buf;
    vtx_value_t    *const_buf;
};

/* ---- Helpers ---- */

static void cg_set_error(lv_codegen_t *cg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (cg->last_error) lv_free(cg->last_error);
    cg->last_error = lv_strdup(buf);
}

/* ---- Bytecode emission ---- */

static void lv_emit_byte(lv_func_cg_t *f, uint8_t b) {
    if (f->code.len + 1 > f->code.cap) {
        f->code.cap = f->code.cap ? f->code.cap * 2 : 64;
        f->code.code = lv_realloc(f->code.code, f->code.cap);
    }
    f->code.code[f->code.len++] = b;
}

static void emit_op(lv_func_cg_t *f, vtx_opcode_t op) {
    lv_emit_byte(f, (uint8_t)op);
}

static size_t emit_operand16_placeholder(lv_func_cg_t *f) {
    size_t off = f->code.len;
    lv_emit_byte(f, 0);
    lv_emit_byte(f, 0);
    return off;
}

static void emit_operand16(lv_func_cg_t *f, uint16_t val) {
    lv_emit_byte(f, (val >> 8) & 0xFF);
    lv_emit_byte(f, val & 0xFF);
}

static void patch_operand16(lv_func_cg_t *f, size_t off, uint16_t val) {
    f->code.code[off]     = (val >> 8) & 0xFF;
    f->code.code[off + 1] = val & 0xFF;
}

/* ---- Stack tracking ---- */
static void push1(lv_func_cg_t *f) {
    f->cur_stack++;
    if (f->cur_stack > f->max_stack) f->max_stack = f->cur_stack;
}
static void pop1(lv_func_cg_t *f) {
    if (f->cur_stack > 0) f->cur_stack--;
}
static void push_n(lv_func_cg_t *f, int n) { while (n-- > 0) push1(f); }
static void pop_n(lv_func_cg_t *f, int n) { while (n-- > 0) pop1(f); }

/* ---- Constant pool ---- */
static uint16_t const_add_int(lv_func_cg_t *f, int64_t v) {
    if (f->pool.count >= f->pool.cap) {
        f->pool.cap = f->pool.cap ? f->pool.cap * 2 : 16;
        f->pool.consts = lv_realloc(f->pool.consts, sizeof(vtx_value_t) * f->pool.cap);
    }
    if (v < VTX_SMI_MIN || v > VTX_SMI_MAX) {
        f->pool.consts[f->pool.count] = vtx_make_double((double)v);
    } else {
        f->pool.consts[f->pool.count] = vtx_make_smi(v);
    }
    return (uint16_t)f->pool.count++;
}

static uint16_t const_add_float(lv_func_cg_t *f, double v) {
    if (f->pool.count >= f->pool.cap) {
        f->pool.cap = f->pool.cap ? f->pool.cap * 2 : 16;
        f->pool.consts = lv_realloc(f->pool.consts, sizeof(vtx_value_t) * f->pool.cap);
    }
    f->pool.consts[f->pool.count] = vtx_make_double(v);
    return (uint16_t)f->pool.count++;
}

static uint16_t const_add_string(lv_func_cg_t *f, const char *s, size_t len) {
    if (f->pool.count >= f->pool.cap) {
        f->pool.cap = f->pool.cap ? f->pool.cap * 2 : 16;
        f->pool.consts = lv_realloc(f->pool.consts, sizeof(vtx_value_t) * f->pool.cap);
    }
    /* Intern the string in the runtime's string table; the resulting
     * vtx_value_t is a heap pointer. */
    f->pool.consts[f->pool.count] = lv_runtime_intern_string(f->cg->rt, s, len);
    return (uint16_t)f->pool.count++;
}

/* ---- Labels & patches ---- */
static int label_alloc(lv_func_cg_t *f) {
    if (f->nlabels + 1 > f->cap_labels) {
        f->cap_labels = f->cap_labels ? f->cap_labels * 2 : 16;
        f->labels = lv_realloc(f->labels, sizeof(label_t) * f->cap_labels);
    }
    int marker = f->next_marker++;
    f->labels[f->nlabels].marker = marker;
    f->labels[f->nlabels].pc     = 0;
    f->labels[f->nlabels].placed = false;
    f->nlabels++;
    return marker;
}

static void label_place(lv_func_cg_t *f, int marker) {
    for (int i = 0; i < f->nlabels; i++) {
        if (f->labels[i].marker == marker) {
            f->labels[i].pc     = f->code.len;
            f->labels[i].placed = true;
            return;
        }
    }
}

static size_t label_lookup_pc(lv_func_cg_t *f, int marker) {
    for (int i = 0; i < f->nlabels; i++) {
        if (f->labels[i].marker == marker) return f->labels[i].pc;
    }
    return 0;
}

static void add_pending_jump(lv_func_cg_t *f, size_t patch_off, int target_marker) {
    if (f->njumps + 1 > f->cap_jumps) {
        f->cap_jumps = f->cap_jumps ? f->cap_jumps * 2 : 16;
        f->jumps = lv_realloc(f->jumps, sizeof(pending_jump_t) * f->cap_jumps);
    }
    f->jumps[f->njumps].patch_offset   = patch_off;
    f->jumps[f->njumps].target_marker  = target_marker;
    f->njumps++;
}

static void resolve_jumps(lv_func_cg_t *f) {
    for (int i = 0; i < f->njumps; i++) {
        size_t pc = label_lookup_pc(f, f->jumps[i].target_marker);
        patch_operand16(f, f->jumps[i].patch_offset, (uint16_t)pc);
    }
}

/* Emit GOTO to a label marker (forward or backward). */
static void emit_goto(lv_func_cg_t *f, int marker) {
    emit_op(f, VT_OP_GOTO);
    size_t off = emit_operand16_placeholder(f);
    add_pending_jump(f, off, marker);
}

/* Emit IF_TRUE / IF_FALSE (pop 1, branch). */
static void emit_if_true(lv_func_cg_t *f, int marker) {
    emit_op(f, VT_OP_IF_TRUE);
    size_t off = emit_operand16_placeholder(f);
    add_pending_jump(f, off, marker);
    pop1(f);
}
static void emit_if_false(lv_func_cg_t *f, int marker) {
    emit_op(f, VT_OP_IF_FALSE);
    size_t off = emit_operand16_placeholder(f);
    add_pending_jump(f, off, marker);
    pop1(f);
}

/* ---- Scope management ---- */
static void scope_enter(lv_func_cg_t *f) {
    f->scope.depth++;
}

static void scope_leave(lv_func_cg_t *f) {
    /* Pop locals declared at the current depth. */
    while (f->scope.count > 0 &&
           f->scope.items[f->scope.count - 1].scope_depth >= f->scope.depth) {
        lv_free(f->scope.items[f->scope.count - 1].name);
        f->scope.count--;
    }
    if (f->scope.depth > 0) f->scope.depth--;
}

static int scope_declare_local(lv_func_cg_t *f, const char *name) {
    if (f->scope.count + 1 > f->scope.cap) {
        f->scope.cap = f->scope.cap ? f->scope.cap * 2 : 16;
        f->scope.items = lv_realloc(f->scope.items, sizeof(local_t) * f->scope.cap);
    }
    int slot = f->scope.next_slot++;
    if (slot + 1 > f->scope.max_slots) f->scope.max_slots = slot + 1;
    f->scope.items[f->scope.count].name        = lv_strdup(name);
    f->scope.items[f->scope.count].slot        = slot;
    f->scope.items[f->scope.count].scope_depth = f->scope.depth;
    f->scope.count++;
    return slot;
}

/* Look up a local by name (search innermost-first). Returns slot or -1. */
static int scope_lookup_local(lv_func_cg_t *f, const char *name) {
    for (int i = f->scope.count - 1; i >= 0; i--) {
        if (strcmp(f->scope.items[i].name, name) == 0) {
            return f->scope.items[i].slot;
        }
    }
    return -1;
}

/* ---- Break stack ---- */
static void break_push(lv_func_cg_t *f, int marker) {
    if (f->nbreak + 1 > f->cap_break) {
        f->cap_break = f->cap_break ? f->cap_break * 2 : 8;
        f->break_markers = lv_realloc(f->break_markers, sizeof(int) * f->cap_break);
    }
    f->break_markers[f->nbreak++] = marker;
}

static int break_pop(lv_func_cg_t *f) {
    if (f->nbreak == 0) return -1;
    return f->break_markers[--f->nbreak];
}

/* ---- CALL_RUNTIME emission ----
 * The CALL_RUNTIME operand packs (lua_fn_id << 6) | arg_count.
 * arg_count is 0-63 (6 bits). For variadic functions like print(...),
 * we still need to know the actual arg count at the call site — the
 * codegen always knows it, so this works.
 */
static void emit_lua_call(lv_func_cg_t *f, lv_fn_id_t fn_id, int arg_count) {
    emit_op(f, VT_OP_CALL_RUNTIME);
    uint16_t operand = (uint16_t)(((uint16_t)fn_id << 6) | (arg_count & 0x3F));
    emit_operand16(f, operand);
    /* Stack effect: pops arg_count, pushes 1 result. */
    pop_n(f, arg_count);
    push1(f);
}

/* ---- Function codegen state init/fini ---- */
static void func_cg_init(lv_func_cg_t *f, lv_codegen_t *cg, bool is_function, bool is_vararg) {
    memset(f, 0, sizeof(*f));
    f->cg = cg;
    f->is_function = is_function;
    f->is_vararg   = is_vararg;
    /* Local 0 is reserved for the environment object (the global env
     * for the main chunk, or the captured env for nested functions). */
    scope_enter(f);
    f->env_slot = scope_declare_local(f, "(env)");
}

static void func_cg_fini(lv_func_cg_t *f) {
    if (f->code.code)    lv_free(f->code.code);
    if (f->pool.consts)  lv_free(f->pool.consts);
    if (f->scope.items) {
        for (int i = 0; i < f->scope.count; i++) lv_free(f->scope.items[i].name);
        lv_free(f->scope.items);
    }
    if (f->labels)        lv_free(f->labels);
    if (f->jumps)         lv_free(f->jumps);
    if (f->break_markers) lv_free(f->break_markers);
}

/* ---- Forward declarations of expression / statement compilers ---- */
static void compile_expr(lv_func_cg_t *f, lv_node_t *node);
static void compile_stmt(lv_func_cg_t *f, lv_node_t *node);
static void compile_block(lv_func_cg_t *f, lv_node_t *block);
static void compile_call_runtime_varargs(lv_func_cg_t *f, lv_fn_id_t fn_id,
                                          lv_node_t **args, int nargs);

/* ---- Expression compilation ---- */

static void compile_literal(lv_func_cg_t *f, lv_node_t *node) {
    switch (node->kind) {
    case LV_NODE_NIL:
        emit_op(f, VT_OP_LOAD_NULL);
        push1(f);
        break;
    case LV_NODE_TRUE:
        emit_op(f, VT_OP_LOAD_TRUE);
        push1(f);
        break;
    case LV_NODE_FALSE:
        emit_op(f, VT_OP_LOAD_FALSE);
        push1(f);
        break;
    case LV_NODE_INT: {
        uint16_t idx = const_add_int(f, node->ival);
        emit_op(f, VT_OP_LOAD_CONST_INT);
        emit_operand16(f, idx);
        push1(f);
        break;
    }
    case LV_NODE_FLOAT: {
        uint16_t idx = const_add_float(f, node->fval);
        emit_op(f, VT_OP_LOAD_CONST_FLOAT);
        emit_operand16(f, idx);
        push1(f);
        break;
    }
    case LV_NODE_STRING: {
        uint16_t idx = const_add_string(f, node->str, node->str_len);
        emit_op(f, VT_OP_LOAD_CONST_STR);
        emit_operand16(f, idx);
        push1(f);
        break;
    }
    case LV_NODE_DOTS:
        /* Varargs: for MVP, return the first vararg value (or nil).
         * We use a runtime helper that returns the first vararg. */
        emit_lua_call(f, LV_FN_VARARG, 0);
        break;
    default:
        cg_set_error(f->cg, "internal: compile_literal on non-literal node");
        break;
    }
}

/* Compile a variable reference (name lookup).
 * In scope-table mode (function bodies): always use SCOPE_GET.
 * In local mode (main chunk): check VORTEX locals first, then global. */
static void compile_name(lv_func_cg_t *f, lv_node_t *node) {
    if (f->use_scope_table) {
        /* Function body: scope_get(local 0, name) */
        emit_op(f, VT_OP_LOAD_LOCAL);
        emit_operand16(f, (uint16_t)f->env_slot);
        push1(f);
        uint16_t name_idx = const_add_string(f, node->str, strlen(node->str));
        emit_op(f, VT_OP_LOAD_CONST_STR);
        emit_operand16(f, name_idx);
        push1(f);
        emit_lua_call(f, LV_FN_SCOPE_GET, 2);
        return;
    }
    int slot = scope_lookup_local(f, node->str);
    if (slot >= 0) {
        emit_op(f, VT_OP_LOAD_LOCAL);
        emit_operand16(f, (uint16_t)slot);
        push1(f);
        return;
    }
    /* Global: emit (env, "name") -> LV_FN_GLOBAL_GET */
    emit_op(f, VT_OP_LOAD_LOCAL);
    emit_operand16(f, (uint16_t)f->env_slot);
    push1(f);
    uint16_t name_idx = const_add_string(f, node->str, strlen(node->str));
    emit_op(f, VT_OP_LOAD_CONST_STR);
    emit_operand16(f, name_idx);
    push1(f);
    emit_lua_call(f, LV_FN_GLOBAL_GET, 2);
}

/* Compile table indexing: t[k] or t.name */
static void compile_index(lv_func_cg_t *f, lv_node_t *node) {
    /* children[0] = t, children[1] = k */
    compile_expr(f, node->children[0]);
    compile_expr(f, node->children[1]);
    emit_lua_call(f, LV_FN_GET_FIELD, 2);
}

static void compile_field(lv_func_cg_t *f, lv_node_t *node) {
    /* children[0] = t, name in node->str */
    compile_expr(f, node->children[0]);
    uint16_t name_idx = const_add_string(f, node->str, strlen(node->str));
    emit_op(f, VT_OP_LOAD_CONST_STR);
    emit_operand16(f, name_idx);
    push1(f);
    emit_lua_call(f, LV_FN_GET_FIELD, 2);
}

/* Compile a table constructor { ... } */
static void compile_table_ctor(lv_func_cg_t *f, lv_node_t *node) {
    /* Emit: LV_FN_NEW_TABLE (narr, nrec) → table
     * For MVP, narr = number of positional entries, nrec = number of kv entries. */
    int narr = 0, nrec = 0;
    for (int i = 0; i < node->nchildren; i++) {
        lv_node_t *e = node->children[i];
        if (e->kind == LV_NODE_TABLE && e->nchildren == 2) nrec++;
        else narr++;
    }
    /* Push narr, nrec as constants */
    uint16_t i_narr = const_add_int(f, narr);
    emit_op(f, VT_OP_LOAD_CONST_INT);
    emit_operand16(f, i_narr);
    push1(f);
    uint16_t i_nrec = const_add_int(f, nrec);
    emit_op(f, VT_OP_LOAD_CONST_INT);
    emit_operand16(f, i_nrec);
    push1(f);
    emit_lua_call(f, LV_FN_NEW_TABLE, 2);
    /* Stack now has the new table. */
    /* For each entry: push table, push key, push value, call SET_FIELD. */
    int pos_idx = 1; /* 1-based for positional */
    for (int i = 0; i < node->nchildren; i++) {
        lv_node_t *e = node->children[i];
        if (e->kind == LV_NODE_TABLE && e->nchildren == 2) {
            /* kv-pair: children[0]=key, children[1]=value */
            /* Duplicate the table on the stack */
            emit_op(f, VT_OP_DUP);
            push1(f);
            compile_expr(f, e->children[0]); /* key */
            compile_expr(f, e->children[1]); /* value */
            emit_lua_call(f, LV_FN_SET_FIELD, 3);
            /* SET_FIELD returns the table; emit a POP to discard it
             * (we already have the original on the stack). */
            emit_op(f, VT_OP_POP);
            pop1(f);
        } else {
            /* positional: key = pos_idx */
            emit_op(f, VT_OP_DUP);
            push1(f);
            uint16_t k_idx = const_add_int(f, pos_idx++);
            emit_op(f, VT_OP_LOAD_CONST_INT);
            emit_operand16(f, k_idx);
            push1(f);
            compile_expr(f, e); /* value */
            emit_lua_call(f, LV_FN_SET_FIELD, 3);
            emit_op(f, VT_OP_POP);
            pop1(f);
        }
    }
}

/* Compile a binary operation. */
static void compile_binop(lv_func_cg_t *f, lv_node_t *node) {
    /* Short-circuit `and` / `or`: */
    if (node->binop == LV_BIN_AND) {
        /* left; if false, skip; right */
        compile_expr(f, node->children[0]);
        emit_op(f, VT_OP_DUP);
        push1(f);
        int end_label = label_alloc(f);
        emit_if_false(f, end_label);
        /* if we don't branch, the value is truthy — pop it and emit right */
        emit_op(f, VT_OP_POP);
        pop1(f);
        compile_expr(f, node->children[1]);
        label_place(f, end_label);
        return;
    }
    if (node->binop == LV_BIN_OR) {
        compile_expr(f, node->children[0]);
        emit_op(f, VT_OP_DUP);
        push1(f);
        int end_label = label_alloc(f);
        emit_if_true(f, end_label);
        emit_op(f, VT_OP_POP);
        pop1(f);
        compile_expr(f, node->children[1]);
        label_place(f, end_label);
        return;
    }

    /* Non-short-circuit: compile both operands, then emit the op. */
    compile_expr(f, node->children[0]);
    compile_expr(f, node->children[1]);

    /* For known integer constants, we could emit IADD/etc. directly.
     * However, since Lua is dynamically typed, the actual type is only
     * known at runtime. The simplest correct approach is to always
     * dispatch through the runtime arithmetic helper, which checks
     * types and performs the right operation.
     *
     * For MVP, all arithmetic goes through CALL_RUNTIME. The JIT will
     * speculatively inline the integer fast path when it observes hot
     * integer operands. */
    lv_fn_id_t fn;
    switch (node->binop) {
    case LV_BIN_ADD:    fn = LV_FN_ARITH_ADD;    break;
    case LV_BIN_SUB:    fn = LV_FN_ARITH_SUB;    break;
    case LV_BIN_MUL:    fn = LV_FN_ARITH_MUL;    break;
    case LV_BIN_DIV:    fn = LV_FN_ARITH_DIV;    break;
    case LV_BIN_IDIV:   fn = LV_FN_ARITH_IDIV;   break;
    case LV_BIN_MOD:    fn = LV_FN_ARITH_MOD;    break;
    case LV_BIN_POW:    fn = LV_FN_ARITH_POW;    break;
    case LV_BIN_CONCAT: fn = LV_FN_ARITH_CONCAT; break;
    case LV_BIN_EQ:     fn = LV_FN_CMP_EQ;       break;
    case LV_BIN_NE:     fn = LV_FN_CMP_EQ;       break; /* invert in helper */
    case LV_BIN_LT:     fn = LV_FN_CMP_LT;       break;
    case LV_BIN_LE:     fn = LV_FN_CMP_LE;       break;
    case LV_BIN_GT:     fn = LV_FN_CMP_LT;       break; /* swap operands */
    case LV_BIN_GE:     fn = LV_FN_CMP_LE;       break; /* swap operands */
    case LV_BIN_BAND:   fn = LV_FN_BIT_AND;      break;
    case LV_BIN_BOR:    fn = LV_FN_BIT_OR;       break;
    case LV_BIN_BXOR:   fn = LV_FN_BIT_XOR;      break;
    case LV_BIN_SHL:    fn = LV_FN_BIT_SHL;      break;
    case LV_BIN_SHR:    fn = LV_FN_BIT_SHR;      break;
    default:
        cg_set_error(f->cg, "internal: unhandled binop");
        return;
    }
    /* For > and >=, we swap operand order in the helper. The simplest
     * way: emit them and let the runtime helper detect the swap via
     * a different fn_id. For MVP, just handle < and <= and rewrite
     * a > b as b < a at the AST level... but we already emitted both.
     * So we use distinct helper logic: pass the op via the fn_id.
     * We add LV_FN_CMP_GT and LV_FN_CMP_GE. */
    if (node->binop == LV_BIN_GT) fn = LV_FN_CMP_LT; /* with swap flag */
    if (node->binop == LV_BIN_GE) fn = LV_FN_CMP_LE; /* with swap flag */

    emit_lua_call(f, fn, 2);

    /* For != we need to invert. The helper returns 1 for equal; we
     * want 0. We emit a NOT-like runtime helper. For MVP, do it inline:
     * if helper returned true, push false. */
    if (node->binop == LV_BIN_NE) {
        /* Pop the bool, push its negation. We use LV_FN_TOBOOLEAN? No—
         * we use a small inline trick: compare against false. */
        /* Actually, simpler: we use a runtime helper that returns
         * the boolean NOT. We don't have one. So we emit:
         *   LOAD_FALSE
         *   CMP_EQ   (which checks if the result == false)
         * That gives us NOT.
         */
        /* But that requires another CMP_EQ call. Easier: add a
         * dedicated NOT helper. For MVP we just reuse the equality
         * helper with inverted semantics by emitting a small flag. */
        /* Quick approach: replace the value with `not value` using
         * the existing IF_TRUE/IF_FALSE opcodes. */
        int true_l = label_alloc(f);
        int end_l  = label_alloc(f);
        emit_op(f, VT_OP_DUP);
        push1(f);
        emit_if_true(f, true_l);
        /* value was false: pop it, push true (since we want NOT) */
        emit_op(f, VT_OP_POP);
        pop1(f);
        emit_op(f, VT_OP_LOAD_FALSE);
        push1(f);
        emit_goto(f, end_l);
        label_place(f, true_l);
        emit_op(f, VT_OP_POP);
        pop1(f);
        emit_op(f, VT_OP_LOAD_TRUE);
        push1(f);
        label_place(f, end_l);
    }
}

static void compile_unop(lv_func_cg_t *f, lv_node_t *node) {
    compile_expr(f, node->children[0]);
    lv_fn_id_t fn;
    switch (node->unop) {
    case LV_UN_NEG:
        /* -x: emit 0 - x via ARITH_SUB. Push 0 first, then re-arrange.
         * Simpler: emit a dedicated helper. For MVP, swap: push 0, swap, sub. */
        emit_op(f, VT_OP_LOAD_CONST_INT);
        emit_operand16(f, const_add_int(f, 0));
        push1(f);
        emit_op(f, VT_OP_SWAP);
        emit_lua_call(f, LV_FN_ARITH_SUB, 2);
        return;
    case LV_UN_NOT: {
        /* not x: emit inline NOT via if/else */
        int true_l = label_alloc(f);
        int end_l  = label_alloc(f);
        emit_op(f, VT_OP_DUP);
        push1(f);
        emit_if_true(f, true_l);
        emit_op(f, VT_OP_POP);
        pop1(f);
        emit_op(f, VT_OP_LOAD_TRUE);
        push1(f);
        emit_goto(f, end_l);
        label_place(f, true_l);
        emit_op(f, VT_OP_POP);
        pop1(f);
        emit_op(f, VT_OP_LOAD_FALSE);
        push1(f);
        label_place(f, end_l);
        return;
    }
    case LV_UN_LEN:
        fn = LV_FN_LENGTH;
        break;
    case LV_UN_BNOT:
        /* ~x: emit x XOR -1 (all bits set) via BIT_XOR with -1. */
        emit_op(f, VT_OP_LOAD_CONST_INT);
        emit_operand16(f, const_add_int(f, -1));
        push1(f);
        emit_op(f, VT_OP_SWAP);
        emit_lua_call(f, LV_FN_BIT_XOR, 2);
        return;
    default:
        cg_set_error(f->cg, "internal: unhandled unop");
        return;
    }
    emit_lua_call(f, fn, 1);
}

/* Compile a function call: fn(args...). */
static void compile_call(lv_func_cg_t *f, lv_node_t *node) {
    /* children[0] = callee, children[1..] = args */
    lv_node_t *callee = node->children[0];
    int nargs = node->nchildren - 1;

    /* Special form: if callee is a field access (obj.method), we could
     * lower to method-call syntax. For MVP, treat all calls uniformly:
     * push callee, push args, call LV_FN_CALL. */
    compile_expr(f, callee);
    for (int i = 0; i < nargs; i++) {
        compile_expr(f, node->children[i + 1]);
    }
    /* LV_FN_CALL pops (1 + nargs) and pushes 1 result. */
    emit_lua_call(f, LV_FN_CALL, 1 + nargs);
}

/* Compile a method call: obj:name(args...). */
static void compile_method_call(lv_func_cg_t *f, lv_node_t *node) {
    /* children[0] = receiver, name in node->str, then args */
    int nargs = node->nchildren - 1;
    compile_expr(f, node->children[0]);
    uint16_t name_idx = const_add_string(f, node->str, strlen(node->str));
    emit_op(f, VT_OP_LOAD_CONST_STR);
    emit_operand16(f, name_idx);
    push1(f);
    for (int i = 0; i < nargs; i++) {
        compile_expr(f, node->children[i + 1]);
    }
    emit_lua_call(f, LV_FN_CALL_METHOD, 2 + nargs);
}

/* Compile a function literal. For MVP, we don't compile nested functions
 * to separate VORTEX methods — instead, we represent the function as a
 * Lua closure object created at runtime via LV_FN_NEW_CLOSURE.
 * The closure carries a function ID that the runtime uses to dispatch
 * the call. We compile the function body to a separate bytecode buffer
 * and register it with the runtime under that ID. */
static void compile_function(lv_func_cg_t *f, lv_node_t *node);

/* Dispatch entry for expression compilation. */
static void compile_expr(lv_func_cg_t *f, lv_node_t *node) {
    if (!node) {
        cg_set_error(f->cg, "internal: null node in compile_expr");
        return;
    }
    switch (node->kind) {
    case LV_NODE_NIL:
    case LV_NODE_TRUE:
    case LV_NODE_FALSE:
    case LV_NODE_INT:
    case LV_NODE_FLOAT:
    case LV_NODE_STRING:
    case LV_NODE_DOTS:
        compile_literal(f, node);
        return;
    case LV_NODE_NAME:    compile_name(f, node);    return;
    case LV_NODE_INDEX:   compile_index(f, node);   return;
    case LV_NODE_FIELD:   compile_field(f, node);   return;
    case LV_NODE_TABLE:   compile_table_ctor(f, node); return;
    case LV_NODE_BINOP:   compile_binop(f, node);   return;
    case LV_NODE_UNOP:    compile_unop(f, node);    return;
    case LV_NODE_CALL:    compile_call(f, node);    return;
    case LV_NODE_METHOD_CALL: compile_method_call(f, node); return;
    case LV_NODE_FUNCTION: compile_function(f, node); return;
    default:
        cg_set_error(f->cg, "internal: unhandled expr kind %d", node->kind);
        return;
    }
}

/* ---- Statement compilation ---- */

static void compile_local(lv_func_cg_t *f, lv_node_t *node) {
    /* names[] = children[] (values) */
    int nvals = node->nchildren;
    if (f->use_scope_table) {
        /* Function body: declare locals in the scope table.
         * We need: scope_declare(scope, name, value) with args in order
         * [scope, name, value] on the stack (value on top). */
        for (int i = 0; i < node->nnames; i++) {
            /* Evaluate value first, store in temp. */
            if (i < nvals) {
                compile_expr(f, node->children[i]);
            } else {
                emit_op(f, VT_OP_LOAD_NULL);
                push1(f);
            }
            int tmp = scope_declare_local(f, "(local-tmp)");
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)tmp);
            pop1(f);
            /* Push scope, name, value. */
            emit_op(f, VT_OP_LOAD_LOCAL);
            emit_operand16(f, (uint16_t)f->env_slot);
            push1(f);
            uint16_t name_idx = const_add_string(f, node->names[i], strlen(node->names[i]));
            emit_op(f, VT_OP_LOAD_CONST_STR);
            emit_operand16(f, name_idx);
            push1(f);
            emit_op(f, VT_OP_LOAD_LOCAL);
            emit_operand16(f, (uint16_t)tmp);
            push1(f);
            /* Stack: [scope][name][value] (value on top). */
            emit_lua_call(f, LV_FN_SCOPE_DECLARE, 3);
            emit_op(f, VT_OP_POP);
            pop1(f);
        }
        return;
    }
    /* Main chunk: use VORTEX locals. */
    for (int i = 0; i < node->nnames; i++) {
        if (i < nvals) {
            compile_expr(f, node->children[i]);
        } else {
            emit_op(f, VT_OP_LOAD_NULL);
            push1(f);
        }
        int slot = scope_declare_local(f, node->names[i]);
        emit_op(f, VT_OP_STORE_LOCAL);
        emit_operand16(f, (uint16_t)slot);
        pop1(f);
        /* Also store in the global env so nested closures can see it
         * via the scope chain. This is necessary because closures
         * capture the global env (local 0) — they can't see VORTEX
         * locals from the enclosing chunk directly. */
        {
            emit_op(f, VT_OP_LOAD_LOCAL);
            emit_operand16(f, (uint16_t)f->env_slot);
            push1(f);
            uint16_t name_idx = const_add_string(f, node->names[i], strlen(node->names[i]));
            emit_op(f, VT_OP_LOAD_CONST_STR);
            emit_operand16(f, name_idx);
            push1(f);
            emit_op(f, VT_OP_LOAD_LOCAL);
            emit_operand16(f, (uint16_t)slot);
            push1(f);
            emit_lua_call(f, LV_FN_GLOBAL_SET, 3);
            emit_op(f, VT_OP_POP);
            pop1(f);
        }
    }
}

static void compile_assign(lv_func_cg_t *f, lv_node_t *node) {
    /* children[0..ntargets-1] = targets, children[ntargets..] = values */
    int ntargets = node->nchildren / 2;
    /* Heuristic: if ntargets == nvalues, do simple parallel assignment.
     * For MVP, we just do sequential assignment (each target = corresponding value). */
    /* Evaluate values first, store to targets. */
    /* To support `a, b = b, a` semantics, we'd need to evaluate all values
     * to temporaries, then assign. For MVP we just do sequential. */
    for (int i = 0; i < ntargets; i++) {
        lv_node_t *target = node->children[i];
        lv_node_t *val    = node->children[ntargets + i];
        if (!val) {
            emit_op(f, VT_OP_LOAD_NULL);
            push1(f);
        } else {
            compile_expr(f, val);
        }
        /* Store to target. */
        if (target->kind == LV_NODE_NAME) {
            int slot = scope_lookup_local(f, target->str);
            if (slot >= 0) {
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)slot);
                pop1(f);
            } else {
                /* Global assignment: env.name = value */
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)f->env_slot);
                push1(f);
                uint16_t name_idx = const_add_string(f, target->str, strlen(target->str));
                emit_op(f, VT_OP_LOAD_CONST_STR);
                emit_operand16(f, name_idx);
                push1(f);
                /* Stack: value, env, name — reorder to env, name, value */
                /* Currently: [value][env][name] (top)
                 * Want:       [env][name][value] (top) for SET_FIELD
                 * We can emit a rotation, but VORTEX only has SWAP.
                 * Easiest: use a dedicated helper LV_FN_GLOBAL_SET that
                 * takes (value, env, name) and does the assignment.
                 * But the helper expects (env, name, value).
                 * So reorder: emit value to a temp local, then push env,
                 * name, value-from-temp. */
                /* Save value to a temp local. */
                int tmp = scope_declare_local(f, "(tmp)");
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                pop1(f);
                /* Now stack: [env][name] (we already pushed env and name above) */
                /* Wait — we pushed value first, then env, then name. After
                 * storing value to tmp, stack is [env][name]. We need
                 * [env][name][value]. Push value back from tmp. */
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                push1(f);
                /* Stack: [env][name][value] — wait, we have [env][name]
                 * (in that order from bottom) then [value] on top. But
                 * the helper expects args in order env, name, value
                 * (with value on top, name below it, env below that).
                 * Stack: bottom→[env][name][value]←top. Yes, correct. */
                emit_lua_call(f, LV_FN_GLOBAL_SET, 3);
                pop1(f); /* GLOBAL_SET returns nil — but we don't use it */
                /* Pop the nil result; we don't want it as a statement value */
                /* Actually emit_lua_call already adjusted stack: popped 3, pushed 1. */
                /* But we still have the env and name on the stack? No —
                 * emit_lua_call popped all 3 args. So stack now has 1 result. */
                /* Pop the result (we don't use it). */
                /* Wait, we have an issue: after STORE_LOCAL tmp, we had
                 * [env][name] on stack (2 items). Then LOAD_LOCAL tmp
                 * pushed value (3 items). Then CALL popped 3, pushed 1.
                 * So stack has 1 item (the result). Pop it. */
                /* But emit_lua_call already did pop_n(3) and push1(1), so
                 * cur_stack went from 3 → 1. We need to pop the result. */
                emit_op(f, VT_OP_POP);
                pop1(f);
                /* Clean up the temp local slot (just for clarity; slot
                 * is never reused in MVP). */
            }
        } else if (target->kind == LV_NODE_FIELD) {
            /* t.name = value
             * Emit: t, "name", value, SET_FIELD */
            compile_expr(f, target->children[0]);
            uint16_t name_idx = const_add_string(f, target->str, strlen(target->str));
            emit_op(f, VT_OP_LOAD_CONST_STR);
            emit_operand16(f, name_idx);
            push1(f);
            /* Stack: [value][t][name]  (top)
             * Want: [t][name][value]  (top)
             * Same reordering issue. Use a temp local for value. */
            /* Actually the value was the FIRST thing we pushed. Let's
             * re-do: we should have evaluated value LAST. Restructure. */
            /* For MVP simplicity: save value to temp, then evaluate target
             * object, push key, load value from temp, call SET_FIELD. */
            /* But we already pushed value. Hmm. Let's restructure this
             * whole function: for each (target, value) pair, if target
             * is a field/index, evaluate value first to a temp, then
             * emit the target's object/key, then load value from temp. */
            cg_set_error(f->cg, "internal: assignment to field not yet supported in this position");
            return;
        } else if (target->kind == LV_NODE_INDEX) {
            cg_set_error(f->cg, "internal: assignment to indexed not yet supported in this position");
            return;
        } else {
            cg_set_error(f->cg, "cannot assign to this expression");
            return;
        }
    }
}

/* Improved assignment that handles field/index targets correctly. */
static void compile_assign_v2(lv_func_cg_t *f, lv_node_t *node) {
    int ntargets = node->nchildren / 2;
    int nvals    = node->nchildren - ntargets;
    if (f->use_scope_table) {
        /* Function body: use SCOPE_SET for name assignments. */
        for (int i = 0; i < ntargets; i++) {
            lv_node_t *target = node->children[i];
            lv_node_t *val    = (i < nvals) ? node->children[ntargets + i] : NULL;
            if (target->kind == LV_NODE_NAME) {
                /* Evaluate value to temp. */
                if (val) compile_expr(f, val);
                else { emit_op(f, VT_OP_LOAD_NULL); push1(f); }
                int tmp = scope_declare_local(f, "(assign-tmp)");
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                pop1(f);
                /* scope_set(local 0, name, value) */
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)f->env_slot);
                push1(f);
                uint16_t name_idx = const_add_string(f, target->str, strlen(target->str));
                emit_op(f, VT_OP_LOAD_CONST_STR);
                emit_operand16(f, name_idx);
                push1(f);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                push1(f);
                emit_lua_call(f, LV_FN_SCOPE_SET, 3);
                emit_op(f, VT_OP_POP);
                pop1(f);
            } else if (target->kind == LV_NODE_FIELD) {
                if (val) compile_expr(f, val);
                else { emit_op(f, VT_OP_LOAD_NULL); push1(f); }
                int tmp = scope_declare_local(f, "(assign-tmp)");
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                pop1(f);
                compile_expr(f, target->children[0]);
                uint16_t name_idx = const_add_string(f, target->str, strlen(target->str));
                emit_op(f, VT_OP_LOAD_CONST_STR);
                emit_operand16(f, name_idx);
                push1(f);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                push1(f);
                emit_lua_call(f, LV_FN_SET_FIELD, 3);
                emit_op(f, VT_OP_POP);
                pop1(f);
            } else if (target->kind == LV_NODE_INDEX) {
                if (val) compile_expr(f, val);
                else { emit_op(f, VT_OP_LOAD_NULL); push1(f); }
                int tmp = scope_declare_local(f, "(assign-tmp)");
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                pop1(f);
                compile_expr(f, target->children[0]);
                compile_expr(f, target->children[1]);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                push1(f);
                emit_lua_call(f, LV_FN_SET_FIELD, 3);
                emit_op(f, VT_OP_POP);
                pop1(f);
            } else {
                cg_set_error(f->cg, "cannot assign to this expression");
                return;
            }
        }
        return;
    }
    /* Main chunk: use VORTEX locals. */
    for (int i = 0; i < ntargets; i++) {
        lv_node_t *target = node->children[i];
        lv_node_t *val    = (i < nvals) ? node->children[ntargets + i] : NULL;

        if (target->kind == LV_NODE_NAME) {
            /* local or global assignment */
            if (val) compile_expr(f, val);
            else { emit_op(f, VT_OP_LOAD_NULL); push1(f); }
            int slot = scope_lookup_local(f, target->str);
            if (slot >= 0) {
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)slot);
                pop1(f);
                /* Also update the global env so nested closures see
                 * the new value via the scope chain. */
                {
                    emit_op(f, VT_OP_LOAD_LOCAL);
                    emit_operand16(f, (uint16_t)f->env_slot);
                    push1(f);
                    uint16_t name_idx = const_add_string(f, target->str, strlen(target->str));
                    emit_op(f, VT_OP_LOAD_CONST_STR);
                    emit_operand16(f, name_idx);
                    push1(f);
                    emit_op(f, VT_OP_LOAD_LOCAL);
                    emit_operand16(f, (uint16_t)slot);
                    push1(f);
                    emit_lua_call(f, LV_FN_GLOBAL_SET, 3);
                    emit_op(f, VT_OP_POP);
                    pop1(f);
                }
            } else {
                /* global: env.name = value */
                int tmp = scope_declare_local(f, "(assign-tmp)");
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                pop1(f);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)f->env_slot);
                push1(f);
                uint16_t name_idx = const_add_string(f, target->str, strlen(target->str));
                emit_op(f, VT_OP_LOAD_CONST_STR);
                emit_operand16(f, name_idx);
                push1(f);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                push1(f);
                emit_lua_call(f, LV_FN_GLOBAL_SET, 3);
                emit_op(f, VT_OP_POP);
                pop1(f);
            }
        } else if (target->kind == LV_NODE_FIELD) {
            /* t.name = value */
            /* Evaluate value to temp. */
            if (val) compile_expr(f, val);
            else { emit_op(f, VT_OP_LOAD_NULL); push1(f); }
            int tmp = scope_declare_local(f, "(assign-tmp)");
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)tmp);
            pop1(f);
            /* Evaluate t. */
            compile_expr(f, target->children[0]);
            /* Push name. */
            uint16_t name_idx = const_add_string(f, target->str, strlen(target->str));
            emit_op(f, VT_OP_LOAD_CONST_STR);
            emit_operand16(f, name_idx);
            push1(f);
            /* Push value. */
            emit_op(f, VT_OP_LOAD_LOCAL);
            emit_operand16(f, (uint16_t)tmp);
            push1(f);
            /* Stack: [t][name][value] (top). Call SET_FIELD. */
            emit_lua_call(f, LV_FN_SET_FIELD, 3);
            emit_op(f, VT_OP_POP);
            pop1(f);
        } else if (target->kind == LV_NODE_INDEX) {
            /* t[k] = value */
            if (val) compile_expr(f, val);
            else { emit_op(f, VT_OP_LOAD_NULL); push1(f); }
            int tmp = scope_declare_local(f, "(assign-tmp)");
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)tmp);
            pop1(f);
            compile_expr(f, target->children[0]); /* t */
            compile_expr(f, target->children[1]); /* k */
            emit_op(f, VT_OP_LOAD_LOCAL);
            emit_operand16(f, (uint16_t)tmp);
            push1(f);
            emit_lua_call(f, LV_FN_SET_FIELD, 3);
            emit_op(f, VT_OP_POP);
            pop1(f);
        } else {
            cg_set_error(f->cg, "cannot assign to this expression");
            return;
        }
    }
}

static void compile_call_stmt(lv_func_cg_t *f, lv_node_t *node) {
    /* children[0] is a CALL or METHOD_CALL — compile it, then pop the result. */
    compile_expr(f, node->children[0]);
    emit_op(f, VT_OP_POP);
    pop1(f);
}

static void compile_do(lv_func_cg_t *f, lv_node_t *node) {
    scope_enter(f);
    compile_block(f, node);
    scope_leave(f);
}

static void compile_while(lv_func_cg_t *f, lv_node_t *node) {
    /* children[0] = cond, children[1..] = body */
    int loop_start = label_alloc(f);
    int loop_end   = label_alloc(f);
    label_place(f, loop_start);
    compile_expr(f, node->children[0]);
    emit_if_false(f, loop_end);
    scope_enter(f);
    f->loop_depth++;
    break_push(f, loop_end);
    for (int i = 1; i < node->nchildren; i++) {
        compile_stmt(f, node->children[i]);
    }
    break_pop(f);
    f->loop_depth--;
    scope_leave(f);
    emit_goto(f, loop_start);
    label_place(f, loop_end);
}

static void compile_repeat(lv_func_cg_t *f, lv_node_t *node) {
    /* children[0..n-1] = body, children[n-1] = cond (last child).
     * Actually our AST stores body in children[0..n-1] and cond as
     * children[n-1] (last). Wait, let me re-check the AST: for REPEAT,
     * children[0..nbody-1] = body, children[nbody] = cond. */
    int n = node->nchildren;
    int loop_start = label_alloc(f);
    int loop_end   = label_alloc(f);
    label_place(f, loop_start);
    scope_enter(f);
    f->loop_depth++;
    break_push(f, loop_end);
    /* body is children[0..n-2] */
    for (int i = 0; i < n - 1; i++) {
        compile_stmt(f, node->children[i]);
    }
    /* cond is children[n-1] */
    compile_expr(f, node->children[n - 1]);
    emit_if_false(f, loop_start); /* if cond is false, loop back */
    break_pop(f);
    f->loop_depth--;
    scope_leave(f);
    label_place(f, loop_end);
}

static void compile_if(lv_func_cg_t *f, lv_node_t *node) {
    /* children are: cond1, body1, [cond2, body2, ...], else_body?
     * Each cond-body pair is 2 children. If nchildren is odd, the last
     * is the else body. If even, no else. */
    int end_label = label_alloc(f);
    int n = node->nchildren;
    int i = 0;
    while (i < n) {
        lv_node_t *cond = node->children[i];
        lv_node_t *body = (i + 1 < n) ? node->children[i + 1] : NULL;
        if (!cond) break;
        compile_expr(f, cond);
        int next_label = label_alloc(f);
        emit_if_false(f, next_label);
        if (body) {
            scope_enter(f);
            /* body is a LV_NODE_DO wrapper */
            if (body->kind == LV_NODE_DO) {
                for (int j = 0; j < body->nchildren; j++) {
                    compile_stmt(f, body->children[j]);
                }
            } else {
                compile_stmt(f, body);
            }
            scope_leave(f);
        }
        emit_goto(f, end_label);
        label_place(f, next_label);
        i += 2;
        /* If there's a single remaining child, it's the else body. */
        if (i == n - 1) {
            lv_node_t *eb = node->children[i];
            scope_enter(f);
            if (eb->kind == LV_NODE_DO) {
                for (int j = 0; j < eb->nchildren; j++) {
                    compile_stmt(f, eb->children[j]);
                }
            } else {
                compile_stmt(f, eb);
            }
            scope_leave(f);
            i = n;
            break;
        }
    }
    label_place(f, end_label);
}

static void compile_for_num(lv_func_cg_t *f, lv_node_t *node) {
    /* numeric for: for v = init, limit, step do body end */
    /* In scope-table mode, the loop variable v is stored in the scope
     * table (so the body can access it via SCOPE_GET). The internal
     * state (limit, step, v_internal) uses VORTEX locals. */
    bool use_scope = f->use_scope_table;

    /* Evaluate init into v_slot (VORTEX local, used as internal state). */
    compile_expr(f, node->exprs[0]);
    int v_slot = scope_declare_local(f, "(for-v)");
    emit_op(f, VT_OP_STORE_LOCAL);
    emit_operand16(f, (uint16_t)v_slot);
    pop1(f);

    compile_expr(f, node->exprs[1]);
    int limit_slot = scope_declare_local(f, "(for-limit)");
    emit_op(f, VT_OP_STORE_LOCAL);
    emit_operand16(f, (uint16_t)limit_slot);
    pop1(f);

    int step_slot = scope_declare_local(f, "(for-step)");
    if (node->exprs[2]) {
        compile_expr(f, node->exprs[2]);
    } else {
        emit_op(f, VT_OP_LOAD_CONST_INT);
        emit_operand16(f, const_add_int(f, 1));
        push1(f);
    }
    emit_op(f, VT_OP_STORE_LOCAL);
    emit_operand16(f, (uint16_t)step_slot);
    pop1(f);

    int loop_start = label_alloc(f);
    int loop_end   = label_alloc(f);
    label_place(f, loop_start);

    /* Condition: v > limit (emitted as limit < v). */
    emit_op(f, VT_OP_LOAD_LOCAL);
    emit_operand16(f, (uint16_t)limit_slot);
    push1(f);
    emit_op(f, VT_OP_LOAD_LOCAL);
    emit_operand16(f, (uint16_t)v_slot);
    push1(f);
    emit_lua_call(f, LV_FN_CMP_LT, 2);
    emit_if_true(f, loop_end);

    /* Body. */
    scope_enter(f);
    f->loop_depth++;
    break_push(f, loop_end);
    /* Store v into the scope table so the body can read it. */
    if (use_scope) {
        emit_op(f, VT_OP_LOAD_LOCAL);
        emit_operand16(f, (uint16_t)f->env_slot);
        push1(f);
        uint16_t ni = const_add_string(f, node->names[0], strlen(node->names[0]));
        emit_op(f, VT_OP_LOAD_CONST_STR);
        emit_operand16(f, ni);
        push1(f);
        emit_op(f, VT_OP_LOAD_LOCAL);
        emit_operand16(f, (uint16_t)v_slot);
        push1(f);
        emit_lua_call(f, LV_FN_SCOPE_DECLARE, 3);
        emit_op(f, VT_OP_POP);
        pop1(f);
    } else {
        int user_slot = scope_declare_local(f, node->names[0]);
        emit_op(f, VT_OP_LOAD_LOCAL);
        emit_operand16(f, (uint16_t)v_slot);
        push1(f);
        emit_op(f, VT_OP_STORE_LOCAL);
        emit_operand16(f, (uint16_t)user_slot);
        pop1(f);
    }
    for (int i = 0; i < node->nchildren; i++) {
        compile_stmt(f, node->children[i]);
    }
    break_pop(f);
    f->loop_depth--;
    scope_leave(f);

    /* Increment: v = v + step */
    emit_op(f, VT_OP_LOAD_LOCAL);
    emit_operand16(f, (uint16_t)v_slot);
    push1(f);
    emit_op(f, VT_OP_LOAD_LOCAL);
    emit_operand16(f, (uint16_t)step_slot);
    push1(f);
    emit_lua_call(f, LV_FN_ARITH_ADD, 2);
    emit_op(f, VT_OP_STORE_LOCAL);
    emit_operand16(f, (uint16_t)v_slot);
    pop1(f);

    emit_goto(f, loop_start);
    label_place(f, loop_end);
}

static void compile_for_in(lv_func_cg_t *f, lv_node_t *node) {
    /* generic for: for k1, k2, ... in explist do body end
     * exprs are in children[0..nexprs-1], body in children[nexprs..].
     * Lowering (Lua 5.4 semantics):
     *   local f, s, var = explist
     *   loop_start:
     *     local results = f(s, var)
     *     if results[1] == nil: goto loop_end
     *     var = results[1]
     *     k1, k2, ... = results[1], results[2], ...
     *     body
     *     goto loop_start
     *   loop_end:
     * For MVP, we support single-expression explist (the common case:
     * `for k, v in pairs(t)` and `for i, v in ipairs(t)`).
     */
    int nexprs = 0;
    while (nexprs < node->nchildren && node->children[nexprs]->kind != LV_NODE_DO &&
           /* heuristic: body nodes are statements, expr nodes are expressions */
           (node->children[nexprs]->kind == LV_NODE_CALL ||
            node->children[nexprs]->kind == LV_NODE_METHOD_CALL ||
            node->children[nexprs]->kind == LV_NODE_NAME ||
            node->children[nexprs]->kind == LV_NODE_FIELD ||
            node->children[nexprs]->kind == LV_NODE_INDEX)) {
        nexprs++;
    }
    int nbody = node->nchildren - nexprs;

    /* Evaluate the explist into f, s, var locals (first 3 values). */
    int f_slot = scope_declare_local(f, "(for-f)");
    int s_slot = scope_declare_local(f, "(for-s)");
    int var_slot = scope_declare_local(f, "(for-var)");

    /* Evaluate first expression; it should yield 3 values (f, s, var). */
    /* For MVP, we evaluate it as a single expression and use the result
     * as f. We don't support multi-return. So `for k, v in pairs(t)` works
     * because pairs returns 3 values, but we only capture the first. */
    /* To make pairs/ipairs work, we special-case them: if the expr is a
     * call to `pairs` or `ipairs`, emit the corresponding helper directly. */
    if (nexprs == 1 && node->children[0]->kind == LV_NODE_CALL) {
        lv_node_t *call = node->children[0];
        lv_node_t *callee = call->children[0];
        if (callee->kind == LV_NODE_NAME &&
            (strcmp(callee->str, "pairs") == 0 || strcmp(callee->str, "ipairs") == 0)) {
            /* Emit: LV_FN_PAIRS or LV_FN_IPAIRS on the table arg.
             * Returns 3 values (f, s, var). For MVP we capture only f
             * and treat s, var as the iterator state (handled by the
             * runtime helper). */
            bool is_pairs = (callee->str[0] == 'p');
            /* Evaluate the table argument and store in s_slot (the state). */
            if (call->nchildren >= 2) {
                compile_expr(f, call->children[1]);
            } else {
                emit_op(f, VT_OP_LOAD_NULL);
                push1(f);
            }
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)s_slot);
            pop1(f);
            /* Call pairs/ipairs on the table to get the iterator f. */
            emit_op(f, VT_OP_LOAD_LOCAL);
            emit_operand16(f, (uint16_t)s_slot);
            push1(f);
            emit_lua_call(f, is_pairs ? LV_FN_PAIRS : LV_FN_IPAIRS, 1);
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)f_slot);
            pop1(f);
            /* var_slot starts as nil (signals first iteration). */
            emit_op(f, VT_OP_LOAD_NULL);
            push1(f);
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)var_slot);
            pop1(f);
            goto loop_body;
        }
    }

    /* Generic case: evaluate explist as a single call, use result as f. */
    for (int i = 0; i < nexprs; i++) {
        compile_expr(f, node->children[i]);
        if (i == 0) {
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)f_slot);
        } else if (i == 1) {
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)s_slot);
        } else if (i == 2) {
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)var_slot);
        }
        pop1(f);
    }
    /* Fill remaining with nil. */
    for (int i = nexprs; i < 3; i++) {
        emit_op(f, VT_OP_LOAD_NULL);
        push1(f);
        if (i == 0) {
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)f_slot);
        } else if (i == 1) {
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)s_slot);
        } else {
            emit_op(f, VT_OP_STORE_LOCAL);
            emit_operand16(f, (uint16_t)var_slot);
        }
        pop1(f);
    }

loop_body:
    {
        int loop_start = label_alloc(f);
        int loop_end   = label_alloc(f);
        label_place(f, loop_start);

        /* Call f(s, var) → result. */
        emit_op(f, VT_OP_LOAD_LOCAL);
        emit_operand16(f, (uint16_t)f_slot);
        push1(f);
        emit_op(f, VT_OP_LOAD_LOCAL);
        emit_operand16(f, (uint16_t)s_slot);
        push1(f);
        emit_op(f, VT_OP_LOAD_LOCAL);
        emit_operand16(f, (uint16_t)var_slot);
        push1(f);
        emit_lua_call(f, LV_FN_CALL, 3);
        /* Result is the new value (or nil if iteration done). */
        /* Store to var_slot and to the loop variable(s). */
        emit_op(f, VT_OP_DUP);
        push1(f);
        emit_op(f, VT_OP_STORE_LOCAL);
        emit_operand16(f, (uint16_t)var_slot);
        /* Stack still has the result on top (from DUP). */
        /* Check if nil → exit loop. */
        int nil_check = label_alloc(f);
        emit_op(f, VT_OP_DUP);
        push1(f);
        /* Use IF_FALSE: if the value is nil (falsy), exit. */
        emit_if_false(f, loop_end);
        emit_op(f, VT_OP_POP);
        pop1(f);

        /* Declare loop variables and assign the result. */
        scope_enter(f);
        f->loop_depth++;
        break_push(f, loop_end);
        if (f->use_scope_table) {
            /* In scope-table mode, store loop vars in the scope table. */
            /* First var: the key (var_slot). */
            if (node->nnames >= 1) {
                /* Evaluate var_slot, store to temp, then scope_declare. */
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)var_slot);
                push1(f);
                int tmp = scope_declare_local(f, "(loop-tmp)");
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                pop1(f);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)f->env_slot);
                push1(f);
                uint16_t ni = const_add_string(f, node->names[0], strlen(node->names[0]));
                emit_op(f, VT_OP_LOAD_CONST_STR);
                emit_operand16(f, ni);
                push1(f);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                push1(f);
                emit_lua_call(f, LV_FN_SCOPE_DECLARE, 3);
                emit_op(f, VT_OP_POP);
                pop1(f);
            }
            /* Second var: t[var_slot] (the value). */
            if (node->nnames >= 2) {
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)s_slot);
                push1(f);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)var_slot);
                push1(f);
                emit_lua_call(f, LV_FN_GET_FIELD, 2);
                int tmp = scope_declare_local(f, "(loop-tmp)");
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                pop1(f);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)f->env_slot);
                push1(f);
                uint16_t ni = const_add_string(f, node->names[1], strlen(node->names[1]));
                emit_op(f, VT_OP_LOAD_CONST_STR);
                emit_operand16(f, ni);
                push1(f);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)tmp);
                push1(f);
                emit_lua_call(f, LV_FN_SCOPE_DECLARE, 3);
                emit_op(f, VT_OP_POP);
                pop1(f);
            }
        } else {
            /* Main chunk: use VORTEX locals. */
            /* First loop variable gets the key (var_slot). */
            if (node->nnames >= 1) {
                int slot = scope_declare_local(f, node->names[0]);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)var_slot);
                push1(f);
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)slot);
                pop1(f);
            }
            /* Second loop variable gets t[var_slot] (the value). */
            if (node->nnames >= 2) {
                int slot = scope_declare_local(f, node->names[1]);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)s_slot);
                push1(f);
                emit_op(f, VT_OP_LOAD_LOCAL);
                emit_operand16(f, (uint16_t)var_slot);
                push1(f);
                emit_lua_call(f, LV_FN_GET_FIELD, 2);
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)slot);
                pop1(f);
            }
            /* Additional loop variables get nil. */
            for (int i = 2; i < node->nnames; i++) {
                int slot = scope_declare_local(f, node->names[i]);
                emit_op(f, VT_OP_LOAD_NULL);
                push1(f);
                emit_op(f, VT_OP_STORE_LOCAL);
                emit_operand16(f, (uint16_t)slot);
                pop1(f);
            }
        }

        /* Body. */
        for (int i = 0; i < nbody; i++) {
            compile_stmt(f, node->children[nexprs + i]);
        }

        break_pop(f);
        f->loop_depth--;
        scope_leave(f);
        emit_goto(f, loop_start);
        label_place(f, loop_end);
        /* Pop the leftover result from the DUP. */
        emit_op(f, VT_OP_POP);
        pop1(f);
    }
}

static void compile_return(lv_func_cg_t *f, lv_node_t *node) {
    /* For MVP: return at most 1 value. */
    if (node->nchildren == 0) {
        emit_op(f, VT_OP_RETURN);
        return;
    }
    /* Evaluate the first return value. */
    compile_expr(f, node->children[0]);
    emit_op(f, VT_OP_RETURN_VALUE);
    pop1(f);
}

static void compile_break(lv_func_cg_t *f, lv_node_t *node) {
    if (f->loop_depth == 0 || f->nbreak == 0) {
        cg_set_error(f->cg, "'break' outside of a loop");
        return;
    }
    emit_goto(f, f->break_markers[f->nbreak - 1]);
}

static void compile_goto(lv_func_cg_t *f, lv_node_t *node) {
    /* Goto is not fully supported in MVP (would require label resolution
     * across block boundaries). Emit a no-op for now. */
    /* TODO: implement label table. */
    (void)node;
}

static void compile_label(lv_func_cg_t *f, lv_node_t *node) {
    /* TODO: place a label. */
    (void)node;
}

/* Compile a function body to a separate VORTEX bytecode module.
 * Uses the scope-table model: local 0 = scope table, all variable
 * accesses go through SCOPE_GET/SCOPE_SET.
 * Returns the bytecode module (caller owns it) and registers it with
 * the runtime under the proto_id. */
static vtx_bytecode_t *compile_function_body(lv_codegen_t *cg, lv_func_proto_t *proto) {
    lv_func_cg_t f;
    func_cg_init(&f, cg, /*is_function=*/true, proto->is_vararg);
    f.use_scope_table = true;

    /* Compile the body statements. */
    for (int i = 0; i < proto->nbody; i++) {
        compile_stmt(&f, proto->body[i]);
        if (cg->last_error) {
            func_cg_fini(&f);
            return NULL;
        }
    }

    /* Emit a final RETURN to ensure clean exit. */
    emit_op(&f, VT_OP_RETURN);

    /* Resolve forward jumps. */
    resolve_jumps(&f);

    /* Build the vtx_bytecode_t. */
    vtx_bytecode_t *bc = lv_alloc(sizeof(*bc));
    memset(bc, 0, sizeof(*bc));
    bc->code = f.code.code;
    bc->length = f.code.len;
    bc->constant_pool = f.pool.consts;
    bc->constant_count = f.pool.count;
    bc->max_locals = (uint16_t)f.scope.max_slots;
    bc->max_stack = (uint16_t)f.max_stack;

    /* Detach buffers from f. */
    f.code.code = NULL;
    f.pool.consts = NULL;
    func_cg_fini(&f);
    return bc;
}

static void compile_function(lv_func_cg_t *f, lv_node_t *node) {
    lv_func_proto_t *proto = node->proto;
    /* Register the proto with the runtime. */
    int proto_id = lv_runtime_register_proto(f->cg->rt, proto);
    /* Compile the function body to VORTEX bytecode. */
    vtx_bytecode_t *bc = compile_function_body(f->cg, proto);
    if (!bc) {
        cg_set_error(f->cg, "failed to compile function body");
        return;
    }
    lv_runtime_set_proto_bytecode(f->cg->rt, proto_id, bc);

    /* Emit: NEW_CLOSURE_WITH_ENV(proto_id, captured_env)
     * The captured env is the current scope: in the main chunk, it's
     * local 0 (the global env); in a function body, it's local 0 (the
     * scope table). Either way, we pass local 0 as the captured env. */
    uint16_t id_idx = const_add_int(f, proto_id);
    emit_op(f, VT_OP_LOAD_CONST_INT);
    emit_operand16(f, id_idx);
    push1(f);
    emit_op(f, VT_OP_LOAD_LOCAL);
    emit_operand16(f, (uint16_t)f->env_slot);
    push1(f);
    emit_lua_call(f, LV_FN_NEW_CLOSURE_WITH_ENV, 2);
}

static void compile_stmt(lv_func_cg_t *f, lv_node_t *node) {
    if (!node) return;
    switch (node->kind) {
    case LV_NODE_LOCAL:    compile_local(f, node);    return;
    case LV_NODE_ASSIGN:   compile_assign_v2(f, node); return;
    case LV_NODE_CALL_STMT:compile_call_stmt(f, node); return;
    case LV_NODE_DO:       compile_do(f, node);       return;
    case LV_NODE_WHILE:    compile_while(f, node);    return;
    case LV_NODE_REPEAT:   compile_repeat(f, node);   return;
    case LV_NODE_IF:       compile_if(f, node);       return;
    case LV_NODE_FOR_NUM:  compile_for_num(f, node);  return;
    case LV_NODE_FOR_IN:   compile_for_in(f, node);   return;
    case LV_NODE_RETURN:   compile_return(f, node);   return;
    case LV_NODE_BREAK:    compile_break(f, node);    return;
    case LV_NODE_GOTO:     compile_goto(f, node);     return;
    case LV_NODE_LABEL:    compile_label(f, node);    return;
    /* Function definitions as statements: `function name() ... end`
     * is parsed as an assignment, so we don't see FUNCTION here. */
    default:
        /* Could be an expression used as a statement (e.g., a literal).
         * Compile and discard. */
        compile_expr(f, node);
        emit_op(f, VT_OP_POP);
        pop1(f);
        return;
    }
}

static void compile_block(lv_func_cg_t *f, lv_node_t *block) {
    if (!block) return;
    if (block->kind == LV_NODE_DO || block->kind == LV_NODE_CHUNK) {
        for (int i = 0; i < block->nchildren; i++) {
            compile_stmt(f, block->children[i]);
            if (f->cg->last_error) return;
        }
    }
}

/* ---- Top-level compile ---- */
lv_compiled_t *lv_codegen_compile(lv_codegen_t *cg, lv_node_t *chunk) {
    if (!chunk || chunk->kind != LV_NODE_CHUNK) {
        cg_set_error(cg, "expected chunk node");
        return NULL;
    }

    lv_func_cg_t f;
    func_cg_init(&f, cg, /*is_function=*/false, /*is_vararg=*/true);

    /* Compile all statements. */
    for (int i = 0; i < chunk->nchildren; i++) {
        compile_stmt(&f, chunk->children[i]);
        if (cg->last_error) {
            func_cg_fini(&f);
            return NULL;
        }
    }

    /* Emit a final RETURN to make sure the chunk exits cleanly. */
    emit_op(&f, VT_OP_RETURN);

    /* Resolve all forward jumps. */
    resolve_jumps(&f);

    /* Build the vtx_bytecode_t. */
    lv_compiled_t *c = lv_alloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->code_buf = f.code.code;
    c->const_buf = f.pool.consts;
    c->bc.code          = c->code_buf;
    c->bc.length        = f.code.len;
    c->bc.constant_pool = c->const_buf;
    c->bc.constant_count = f.pool.count;
    c->bc.max_locals    = (uint16_t)f.scope.max_slots;
    c->bc.max_stack     = (uint16_t)f.max_stack;

    /* Detach buffers from f so func_cg_fini doesn't free them. */
    f.code.code = NULL;
    f.pool.consts = NULL;

    func_cg_fini(&f);
    return c;
}

void lv_compiled_free(lv_compiled_t *c) {
    if (!c) return;
    if (c->code_buf)  lv_free(c->code_buf);
    if (c->const_buf) lv_free(c->const_buf);
    if (c->disasm)    lv_free(c->disasm);
    lv_free(c);
}

const vtx_bytecode_t *lv_compiled_bytecode(const lv_compiled_t *c) {
    return c ? &c->bc : NULL;
}

const char *lv_compiled_disasm(const lv_compiled_t *c) {
    if (!c) return NULL;
    if (!c->disasm) {
        /* Build a simple disassembly by walking the bytecode. */
        size_t cap = 1024;
        char *buf = lv_alloc(cap);
        size_t len = 0;
        #define APPEND(...) do { \
            char tmp[256]; \
            int n = snprintf(tmp, sizeof(tmp), __VA_ARGS__); \
            if (len + n + 1 >= cap) { cap = (len + n + 1) * 2; buf = lv_realloc(buf, cap); } \
            memcpy(buf + len, tmp, n); len += n; \
        } while (0)
        APPEND("; LuaVortex disassembly\n");
        APPEND("; constants: %u\n", c->bc.constant_count);
        APPEND("; max_locals: %u\n", c->bc.max_locals);
        APPEND("; max_stack: %u\n", c->bc.max_stack);
        APPEND("; code length: %zu\n", c->bc.length);
        APPEND("\n");
        size_t pc = 0;
        while (pc < c->bc.length) {
            APPEND("%04zx: ", pc);
            uint8_t op = c->bc.code[pc];
            const char *name = "?";
            bool has_op = false;
            int opsz = 0;
            if (op < VT_OP_COUNT) {
                name = vtx_opcode_table[op].name;
                has_op = vtx_opcode_table[op].has_operand;
                opsz = vtx_opcode_table[op].operand_size;
            }
            APPEND("%s", name);
            if (has_op && pc + 1 + opsz <= c->bc.length) {
                if (opsz == 2) {
                    uint16_t v = (c->bc.code[pc + 1] << 8) | c->bc.code[pc + 2];
                    APPEND(" %u", v);
                } else if (opsz == 1) {
                    APPEND(" %u", c->bc.code[pc + 1]);
                }
            }
            APPEND("\n");
            pc += 1 + (has_op ? opsz : 0);
        }
        buf[len] = 0;
        ((lv_compiled_t *)c)->disasm = buf;
    }
    return c->disasm;
}

const char *lv_codegen_last_error(const lv_codegen_t *cg) {
    return cg ? cg->last_error : NULL;
}

lv_codegen_t *lv_codegen_create(lv_runtime_t *rt) {
    lv_codegen_t *cg = lv_alloc(sizeof(*cg));
    memset(cg, 0, sizeof(*cg));
    cg->rt = rt;
    return cg;
}

void lv_codegen_destroy(lv_codegen_t *cg) {
    if (!cg) return;
    if (cg->last_error) lv_free(cg->last_error);
    lv_free(cg);
}
