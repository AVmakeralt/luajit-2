# Known Bugs and Limitations

## Critical: CALL_RUNTIME callback not invoked at runtime

**Status**: Blocking. The LuaVortex runtime compiles and links, but the
extended CALL_RUNTIME callback is never invoked by the VORTEX interpreter.

**Root cause**: The published `vortex-jit` crate compiles its own copy of
`dispatch.c` (without the LuaVortex hook) into `libvortex.a`. The
`build.rs` compiles a patched copy of `dispatch.c` as a `.o` file and
passes it via `cargo:rustc-link-arg`. However, the linker processes
archives before object files, so the unpatched `vtx_interp_run` from
`libvortex.a` is used instead of the patched version.

**Symptoms**: `print(42)` produces no output. The callback registration
(`vtx_set_runtime_callback`) succeeds, but the VORTEX interpreter's
CALL_RUNTIME handler never calls the callback — it pushes `undefined`
instead.

**Fix**: Push the extended CALL_RUNTIME hook to the VORTEX repo's
`rust-bindings/vendor/interp/dispatch.c`. The patch is in this repo's
`build.rs` (`apply_patch` function). Once the VORTEX crate includes the
hook, remove the `build.rs` workaround and use
`vortex::set_runtime_callback()` directly.

**Workaround**: Use a `[patch]` section in `Cargo.toml` to override the
`vortex-jit` dependency with a local fork that includes the patch.

## Other limitations (same as C version)

1. **Single return value** — functions return at most one value.
2. **No varargs** — `...` evaluates to nil.
3. **No coroutines** — `coroutine` library not implemented.
4. **No metatables** — `setmetatable`/`getmetatable` store the metatable
   but metamethods are not dispatched.
5. **No goto execution** — `goto`/`::labels::` are parsed but emit no-ops.
6. **Mutable upvalues from main chunk** — closures defined in the main
   chunk can read but not write captured locals (see C version's BUGS.md).
7. **No GC integration** — Lua heap objects use `Box`/`Rc`, not VORTEX's GC.
8. **string.gmatch not implemented** — use a manual find+sub loop.
9. **Pattern matching is plain text** — Lua patterns (`%a`, `%d`, etc.)
   are not supported; `string.find`/`match`/`gsub` use plain text search.
10. **table.sort comparator** — custom comparator not supported.
11. **io library limited** — `io.open` returns a file handle but file
    methods are not implemented. `io.read` reads one line from stdin.
