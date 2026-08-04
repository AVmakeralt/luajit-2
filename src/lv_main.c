/* lv_main.c — LuaVortex CLI.
 *
 * Usage:
 *   luavortex [options] [file.lua [args...]]
 *   luavortex -e 'code'
 *
 * Options:
 *   -e CODE    Execute CODE as a Lua chunk
 *   -i         Interactive REPL (after -e or file)
 *   -          Read from stdin
 *   --dump     Print the disassembled bytecode and exit
 *   --no-jit   Disable VORTEX JIT compilation
 *   --tier N   Eagerly compile at tier N (1 or 2)
 *   --help     Show usage
 *   --version  Print version
 */

#include "lv.h"
#include "lv_lexer.h"
#include "lv_parser.h"
#include "lv_codegen.h"
#include "lv_runtime.h"
#include "lv_stdlib.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "LuaVortex %s — a Lua 5.4 frontend for the VORTEX JIT runtime\n"
        "\n"
        "Usage: %s [options] [file.lua [args...]]\n"
        "       %s -e 'code'\n"
        "\n"
        "Options:\n"
        "  -e CODE     Execute CODE as a Lua chunk\n"
        "  -i          Interactive REPL\n"
        "  -           Read from stdin\n"
        "  --dump      Disassemble bytecode and exit (no execution)\n"
        "  --no-jit    Disable VORTEX JIT (interpreter only)\n"
        "  --tier N    Eagerly compile at tier N (1 or 2)\n"
        "  --help      Show this help\n"
        "  --version   Print version and exit\n",
        LV_VERSION_STRING, prog, prog);
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "luavortex: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = lv_alloc(sz + 1);
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    buf[n] = 0;
    if (out_len) *out_len = n;
    return buf;
}

static int run_source(lv_runtime_t *rt, const char *src, size_t src_len,
                      const char *name, bool dump, int tier, bool use_jit) {
    /* Parse. */
    lv_parser_t p;
    lv_parser_init(&p, src, src_len, name);
    lv_node_t *chunk = lv_parser_parse(&p);
    if (!chunk) {
        fprintf(stderr, "luavortex: parse error: %s\n",
                lv_parser_last_error(&p) ? lv_parser_last_error(&p) : "unknown");
        lv_parser_fini(&p);
        return 1;
    }
    if (p.error_count > 0) {
        fprintf(stderr, "luavortex: %d parse error(s): %s\n",
                p.error_count,
                lv_parser_last_error(&p) ? lv_parser_last_error(&p) : "unknown");
        lv_node_free(chunk);
        lv_parser_fini(&p);
        return 1;
    }

    /* Compile to VORTEX bytecode. */
    lv_codegen_t *cg = lv_codegen_create(rt);
    lv_compiled_t *c = lv_codegen_compile(cg, chunk);
    if (!c) {
        fprintf(stderr, "luavortex: compile error: %s\n",
                lv_codegen_last_error(cg) ? lv_codegen_last_error(cg) : "unknown");
        lv_node_free(chunk);
        lv_codegen_destroy(cg);
        lv_parser_fini(&p);
        return 1;
    }

    int rc = 0;
    if (dump) {
        printf("%s\n", lv_compiled_disasm(c));
    } else {
        if (use_jit) {
            vtx_runtime_enable_jit(&rt->vrt, 2);
        }
        if (tier > 0) {
            vtx_method_desc_t m = {};
            m.bytecode = (vtx_bytecode_t *)lv_compiled_bytecode(c);
            m.name = "main";
            m.signature = "()I";
            vtx_runtime_compile(&rt->vrt, &m, tier);
        }
        lv_runtime_run(rt, lv_compiled_bytecode(c));
        if (rt->error_msg) rc = 1;
    }

    lv_compiled_free(c);
    lv_node_free(chunk);
    lv_codegen_destroy(cg);
    lv_parser_fini(&p);
    return rc;
}

int main(int argc, char **argv) {
    const char *exec_str = NULL;
    const char *file = NULL;
    bool interactive = false;
    bool dump = false;
    bool use_jit = true;
    int tier = 0;
    bool from_stdin = false;

    int i = 1;
    while (i < argc) {
        const char *a = argv[i];
        if (strcmp(a, "-e") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 2; }
            exec_str = argv[++i];
        } else if (strcmp(a, "-i") == 0) {
            interactive = true;
        } else if (strcmp(a, "-") == 0) {
            from_stdin = true;
        } else if (strcmp(a, "--dump") == 0) {
            dump = true;
        } else if (strcmp(a, "--no-jit") == 0) {
            use_jit = false;
        } else if (strcmp(a, "--tier") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 2; }
            tier = atoi(argv[++i]);
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("LuaVortex %s\n", LV_VERSION_STRING);
            printf("Built on VORTEX JIT runtime\n");
            return 0;
        } else if (a[0] == '-' && a[1] != 0) {
            fprintf(stderr, "luavortex: unknown option '%s'\n", a);
            usage(argv[0]);
            return 2;
        } else {
            file = a;
            i++; /* skip file, remaining are program args */
            break;
        }
        i++;
    }

    /* Create runtime. */
    lv_runtime_t rt;
    if (lv_runtime_create(&rt) != 0) {
        fprintf(stderr, "luavortex: failed to create runtime\n");
        return 1;
    }

    int rc = 0;

    if (exec_str) {
        rc = run_source(&rt, exec_str, strlen(exec_str), "(command line)",
                        dump, tier, use_jit);
    } else if (from_stdin) {
        size_t cap = 65536, len = 0;
        char *buf = lv_alloc(cap);
        int ch;
        while ((ch = fgetc(stdin)) != EOF) {
            if (len + 1 >= cap) { cap *= 2; buf = lv_realloc(buf, cap); }
            buf[len++] = (char)ch;
        }
        buf[len] = 0;
        rc = run_source(&rt, buf, len, "(stdin)", dump, tier, use_jit);
        lv_free(buf);
    } else if (file) {
        size_t len;
        char *src = read_file(file, &len);
        if (!src) { rc = 1; goto done; }
        rc = run_source(&rt, src, len, file, dump, tier, use_jit);
        lv_free(src);
    } else if (!interactive) {
        /* No input — show usage. */
        usage(argv[0]);
        rc = 2;
    }

    if (interactive) {
        /* Simple REPL. */
        char line[4096];
        printf("LuaVortex %s — type 'exit' to quit\n", LV_VERSION_STRING);
        for (;;) {
            printf("> ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) break;
            if (strcmp(line, "exit\n") == 0 || strcmp(line, "exit\r\n") == 0) break;
            size_t len = strlen(line);
            /* Strip trailing newline. */
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
            if (len == 0) continue;
            run_source(&rt, line, len, "(repl)", false, 0, use_jit);
        }
    }

done:
    lv_runtime_destroy(&rt);
    return rc;
}
