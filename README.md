# LuaVortex (Rust)

A Lua 5.4 frontend for the VORTEX JIT runtime, written in Rust for memory safety and multi-file support.

## Status

**Work in progress.** The Rust port is architecturally complete (lexer, parser, AST, codegen, runtime, stdlib) but has a **known build issue** that prevents runtime execution. See [BUGS.md](BUGS.md) for details.

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

## The VORTEX patch problem

The published `vortex-jit` crate doesn't expose an extension point for `CALL_RUNTIME` opcodes with `func_id >= 100`. LuaVortex needs this to implement dynamic dispatch (string ops, table ops, function calls) from bytecode.

The `build.rs` attempts to work around this by compiling a patched copy of `dispatch.c` and linking it via `--allow-multiple-definition`. However, **the linker uses the unpatched version from `libvortex.a` first**, so the callback is never invoked.

**To fix this**, the VORTEX repo needs the extended CALL_RUNTIME hook pushed to its `rust-bindings/` directory. The patch is ready in this repo's `build.rs` — it just needs to be applied upstream.

## Building

```bash
cargo build
./target/debug/luavortex -e 'print("hello")'
```

Requires: Rust 1.70+, GCC, and the VORTEX C headers (auto-fetched via the `vortex-jit` git dependency).
