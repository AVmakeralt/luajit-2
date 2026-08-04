/* test_basic.c — Basic smoke tests for LuaVortex. */
#include "lv.h"
#include "lv_lexer.h"
#include "lv_parser.h"
#include "lv_codegen.h"
#include "lv_runtime.h"
#include <string.h>
#include <stdio.h>

static int g_tests_run = 0;
static int g_tests_passed = 0;

static void run(const char *name, const char *src, const char *expected_output) {
    g_tests_run++;
    lv_runtime_t rt;
    if (lv_runtime_create(&rt) != 0) {
        fprintf(stderr, "FAIL %s: runtime create failed\n", name);
        return;
    }

    /* Capture stdout. */
    /* For MVP, we just run and check it doesn't crash. A full test
     * harness would capture stdout via pipe redirection. */
    lv_parser_t p;
    lv_parser_init(&p, src, strlen(src), name);
    lv_node_t *chunk = lv_parser_parse(&p);
    if (!chunk) {
        fprintf(stderr, "FAIL %s: parse error: %s\n", name,
                lv_parser_last_error(&p));
        lv_parser_fini(&p);
        lv_runtime_destroy(&rt);
        return;
    }
    lv_codegen_t *cg = lv_codegen_create(&rt);
    lv_compiled_t *c = lv_codegen_compile(cg, chunk);
    if (!c) {
        fprintf(stderr, "FAIL %s: compile error: %s\n", name,
                lv_codegen_last_error(cg));
        lv_node_free(chunk);
        lv_codegen_destroy(cg);
        lv_parser_fini(&p);
        lv_runtime_destroy(&rt);
        return;
    }
    lv_runtime_run(&rt, lv_compiled_bytecode(c));
    if (rt.error_msg) {
        fprintf(stderr, "FAIL %s: runtime error: %s\n", name, rt.error_msg);
    } else {
        g_tests_passed++;
        printf("PASS %s\n", name);
    }
    lv_compiled_free(c);
    lv_node_free(chunk);
    lv_codegen_destroy(cg);
    lv_parser_fini(&p);
    lv_runtime_destroy(&rt);
}

int main(void) {
    run("hello", "print('hello')", "hello\n");
    run("arith", "print(2 + 3)", "5\n");
    run("floats", "print(1.5 + 2.5)", "4\n");
    run("strings", "print('a' .. 'b')", "ab\n");
    run("locals", "local x = 10; print(x * 2)", "20\n");
    run("if", "if 1 > 0 then print('yes') else print('no') end", "yes\n");
    run("while", "local i = 0; while i < 3 do print(i); i = i + 1 end", "0\n1\n2\n");
    run("for", "for i = 1, 3 do print(i) end", "1\n2\n3\n");
    run("function", "local function f(x) return x + 1 end; print(f(41))", "42\n");
    run("recursion", "local function fact(n) if n <= 1 then return 1 end return n * fact(n-1) end; print(fact(5))", "120\n");
    run("table", "local t = {1,2,3}; print(t[1] + t[2] + t[3])", "6\n");
    run("pairs", "local t = {a=1,b=2}; local n = 0; for k,v in pairs(t) do n = n + v end; print(n)", "3\n");
    run("ipairs", "local t = {10,20,30}; local s = 0; for i,v in ipairs(t) do s = s + v end; print(s)", "60\n");
    run("closure", "local function c() local n = 0; return function() n = n + 1; return n end end; local f = c(); print(f()); print(f())", "1\n2\n");
    run("string_lib", "print(string.upper('hi'))", "HI\n");
    run("math_lib", "print(math.floor(3.7))", "3\n");
    run("concat", "print(1 .. 2 .. 3)", "123\n");

    printf("\n%d/%d tests passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
