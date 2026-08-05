# LuaVortex (Rust)

A Lua 5.4 frontend for the VORTEX JIT runtime, written in Rust for memory safety and multi-file support.

## Status

**Work in progress.** The Rust port is architecturally complete (lexer, parser, AST, codegen, runtime, stdlib), but still neeeds edge case handling
## Architecture

```
Lua source → Lexer → Parser → AST → Codegen → VORTEX bytecode → Runtime
```

- **Lexer** (`lexer.rs`): Lua 5.4 tokenizer
- **Parser** (`parser.rs`): recursive-descent parser → AST
- **Codegen** (`codegen.rs`): AST → VORTEX bytecode (all code is compiled, no tree-walker)
- **Runtime** (`runtime.rs`): wraps the VORTEX JIT runtime, manages Lua heap objects
- **Stdlib** (`stdlib.rs`): full Lua stdlib in Rust (print, type, string.*, math.*, table.*, io.*, os.*)
- **Multi-file**: `require()` and `dofile()` support with module caching

## Building

```bash
cargo build
./target/debug/luavortex -e 'print("hello")'
```

Requires: Rust 1.70+, GCC, and the VORTEX C headers (auto-fetched via the `vortex-jit` git dependency).
