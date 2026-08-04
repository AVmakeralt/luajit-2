# LuaVortex

A Lua 5.4-syntax frontend that compiles to [VORTEX](https://github.com/AVmakeralt/VORTEX) bytecode and runs on the VORTEX multi-tier JIT runtime.

LuaVortex is **not** a Lua interpreter. It is a compiler frontend: it lexes, parses, and codegens Lua source code into VORTEX bytecode, which is then executed by the VORTEX interpreter (T0) and JIT-compiled by VORTEX's T1 baseline / T2 optimizing / T3 speculative tiers.

## Features

- **Full Lua 5.4 syntax**: locals, functions, closures, tables, control flow (if/elseif/else, while, repeat/until, numeric for, generic for with pairs/ipairs), multiple assignment, varargs.
- **Dynamic typing**: numbers (int/float), strings, booleans, nil, tables, functions. Uses VORTEX's NaN-boxed value representation (SMI for integers, IEEE 754 doubles for floats).
- **VORTEX backend**: the main chunk is compiled to VORTEX bytecode and run through `vtx_runtime_run()`, which dispatches to JIT-compiled code when hot methods are detected.
- **Lua standard library**: `print`, `type`, `tostring`, `tonumber`, `pairs`, `ipairs`, `assert`, `error`, `pcall`, `select`, `rawget`, `rawset`, `rawequal`, `next`, `setmetatable`, `getmetatable`, `unpack`, plus the `math.*`, `string.*`, `table.*`, `io.*`, `os.*` sublibraries.

## Architecture

```
              ┌─────────────┐   ┌──────────┐   ┌──────────┐   ┌──────────────────────┐
 Lua source → │  Lexer      │ → │  Parser  │ → │  AST     │ → │  Codegen             │
              └─────────────┘   └──────────┘   └──────────┘   │  (AST → VORTEX bc)   │
                                                                └──────────┬───────────┘
                                                                           │
                                                                           ▼
                                                                ┌──────────────────────┐
                                                                │  VORTEX Runtime      │
                                                                │  (interp T0 + JIT)   │
                                                                └──────────┬───────────┘
                                                                           │
                                                      extended CALL_RUNTIME │ (lua_fn_id >= 100)
                                                                           ▼
                                                                ┌──────────────────────┐
                                                                │  LuaVortex Runtime   │
                                                                │  + Stdlib (C)        │
                                                                │  + Tree-walker (for  │
                                                                │    nested closures)  │
                                                                └──────────────────────┘
```

### Execution model

1. The **main chunk** is compiled to VORTEX bytecode and run via `vtx_runtime_run()`. Arithmetic, control flow, and local variable access run directly on the VORTEX interpreter (and are eligible for JIT compilation).

2. The **Lua standard library** is implemented in C. The codegen emits `CALL_RUNTIME` opcodes with a packed operand `(lua_fn_id << 6) | arg_count` where `lua_fn_id >= 100`. The vendored VORTEX interpreter is patched (in `vendor/VORTEX/src/interp/dispatch.c`) to dispatch these extended opcodes back into the LuaVortex runtime via a thread-local pointer.

3. **Nested function literals** (closures) are represented as `lv_function_t` objects carrying a proto ID. When called, their bodies are evaluated by a tree-walking interpreter (`lv_eval.c`) that uses the same value model and stdlib. This is a pragmatic MVP choice — a future version will compile each function to its own VORTEX method, allowing the JIT to optimize hot nested functions.

### Value representation

LuaVortex uses VORTEX's NaN-boxed `vtx_value_t` (64 bits) for all Lua values:

| Lua type   | VORTEX representation                |
|------------|--------------------------------------|
| `nil`      | `VTX_VALUE_NULL`                     |
| `true`/`false` | `VTX_VALUE_TRUE` / `VTX_VALUE_FALSE` |
| integer    | SMI (48-bit signed, NaN-boxed)       |
| float      | raw IEEE 754 double                  |
| string     | heap pointer to `lv_string_t`        |
| table      | hash pointer to `lv_table_t`         |
| function   | heap pointer to `lv_function_t`      |

For MVP, Lua heap objects (strings/tables/functions) are allocated with `malloc` and are not tracked by VORTEX's generational GC. A future version will integrate with VORTEX's GC.

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

This produces the `luavortex` executable.

### Requirements

- CMake ≥ 3.16
- A C17 / C++20 compiler (GCC ≥ 11 or Clang ≥ 14)
- pthreads

The build vendors the VORTEX source tree under `vendor/VORTEX/` and compiles it from source. No separate VORTEX installation is required.

## Usage

```bash
# Run a Lua file
./luavortex examples/hello.lua

# Execute a one-liner
./luavortex -e 'print(2 + 2)'

# Read from stdin
echo 'print("hi")' | ./luavortex -

# Interactive REPL
./luavortex -i

# Disassemble the generated bytecode (without executing)
./luavortex --dump examples/fib.lua

# Disable JIT (interpreter only)
./luavortex --no-jit examples/fib.lua

# Eagerly compile at tier 2 (optimizing JIT)
./luavortex --tier 2 examples/fib.lua
```

## Examples

See the `examples/` directory:
- `hello.lua` — basic arithmetic and string operations
- `fib.lua` — recursive and iterative Fibonacci
- `tables.lua` — table constructors, `table.insert/remove/concat/sort`, `pairs`/`ipairs`
- `closures.lua` — closures, higher-order functions (`map`, `filter`, `reduce`)

## Project layout

```
luajit-2/
├── CMakeLists.txt           # Top-level build (builds VORTEX + LuaVortex)
├── README.md
├── LICENSE                  # Apache 2.0
├── src/                     # LuaVortex frontend source
│   ├── lv.h                 # Common definitions
│   ├── lv_lexer.{h,c}       # Lua 5.4 tokenizer
│   ├── lv_ast.{h,c}         # AST node types
│   ├── lv_parser.{h,c}      # Recursive-descent parser
│   ├── lv_codegen.{h,c}     # AST → VORTEX bytecode emitter
│   ├── lv_value.{h,c}       # Lua value model on VORTEX NaN-boxing
│   ├── lv_runtime.{h,c}     # VORTEX runtime wrapper + extended CALL_RUNTIME
│   ├── lv_stdlib.{h,c}      # Lua standard library (C implementation)
│   ├── lv_eval.{h,c}        # Tree-walking evaluator (for nested closures)
│   └── lv_main.c            # CLI entry point
├── vendor/
│   └── VORTEX/              # Vendored VORTEX JIT runtime
│       └── src/interp/dispatch.c  # Patched: extended CALL_RUNTIME hook
├── examples/                # Sample Lua programs
│   ├── hello.lua
│   ├── fib.lua
│   ├── tables.lua
│   └── closures.lua
└── tests/                   # Test suite
```

## The VORTEX patch

The vendored VORTEX interpreter (`vendor/VORTEX/src/interp/dispatch.c`) has a small patch in the `CALL_RUNTIME` handler's `default:` case. When the operand's high bits encode a `lua_fn_id >= 100`, the patch:

1. Extracts `lua_fn_id = operand >> 6` and `arg_count = operand & 0x3F`.
2. Pops `arg_count` values from the VORTEX operand stack into a C array.
3. Calls `lv_runtime_dispatch(g_lv_runtime, lua_fn_id, argv, arg_count)`.
4. Pushes the single result value back onto the stack.

The `g_lv_runtime` thread-local is set by `lv_runtime_run()` before calling `vtx_runtime_run()`. This allows the VORTEX interpreter (which has no knowledge of LuaVortex) to transparently call into the Lua stdlib.

## Limitations (MVP)

- **Single return value**: Lua functions can return multiple values; LuaVortex currently supports only one return value per call.
- **Varargs**: `...` is parsed but always evaluates to `nil` in nested functions.
- **Closures and JIT**: nested function bodies are evaluated by a tree-walker, not compiled to VORTEX bytecode. Only the main chunk is JIT-eligible.
- **GC integration**: Lua heap objects use `malloc`, not VORTEX's generational GC. Long-running programs may leak memory.
- **Metatables**: `setmetatable`/`getmetatable` are implemented but metamethods (`__index`, `__add`, etc.) are not yet dispatched.
- **goto/labels**: parsed but not yet wired into the codegen.
- **Coroutines**: not implemented.

## License

Apache 2.0 (inherited from VORTEX). See `LICENSE`.
