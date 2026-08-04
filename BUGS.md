# Known Bugs and Limitations

This document lists all known bugs, limitations, and missing features in
LuaVortex. It is maintained alongside the codebase and updated as issues
are discovered or fixed.

## Architecture limitations

### 1. Mutable upvalues from the main chunk don't work correctly

**Status**: Known bug, architectural.

When a closure defined in the **main chunk** captures a local variable
and modifies it, the modification is NOT visible to the main chunk.

```lua
local n = 0
local function inc() n = n + 1 end
inc()
print(n)  -- prints 0, should print 1
```

**Root cause**: The main chunk uses VORTEX locals (for JIT optimization).
To make closures work, main-chunk locals are also stored in the global
env. But numbers are stored by value, not reference — so when the closure
modifies `n` via the scope chain (updating the global env copy), the
main chunk's VORTEX local still holds the old value.

**Workaround**: Use a table to hold mutable state shared with closures:
```lua
local state = {n = 0}
local function inc() state.n = state.n + 1 end
inc()
print(state.n)  -- prints 1 correctly
```

**Fix**: Implement proper upvalue cells (like LuaJIT). Captured locals
would be allocated as 1-element "cells" (boxes), and both the enclosing
scope and the closure hold references to the cell. This is a significant
refactor of the codegen.

### 2. Closures defined inside functions DO work correctly

Closures defined inside **function bodies** (not the main chunk) work
correctly, including mutable upvalues. This is because function bodies
use the scope-table model, where each variable is a table field (shared
by reference).

```lua
local function counter()
    local n = 0
    return function() n = n + 1; return n end
end
local c = counter()
print(c())  -- 1
print(c())  -- 2
```

### 3. Single return value

Functions can only return one value. Lua's multi-return semantics
(`return f()`, `local a, b = f()`, `print(f(), g())`) are not supported.

### 4. Varargs limited

`...` is parsed but always evaluates to `nil` inside function bodies.
The `select("#", ...)` and `select(n, ...)` patterns don't work.

### 5. No coroutines

The `coroutine` library is not implemented. Coroutines require either
`ucontext` (deprecated on some platforms), `makecontext`/`swapcontext`,
or a custom stack-switching mechanism.

### 6. No goto/label execution

`goto` and `::labels::` are parsed but not compiled. They emit no-ops.

## Standard library limitations

### 7. string.gmatch not implemented

`string.gmatch` returns an iterator that yields all matches of a pattern.
It's not implemented because it requires stateful iteration (coroutine-
like). Use a manual `string.find` + `string.sub` loop instead.

### 8. string.format limited

`string.format` supports `%d`, `%s`, `%f`, `%g`, `%x`, `%c`, `%%`, and
`%q`. It does NOT support:
- Width/precision specifiers (e.g., `%5.2f`)
- `%e` / `%E` (scientific notation) — partially supported
- Argument indexing (`%1$d`)

### 9. Pattern matching is a subset

Lua patterns are supported with these limitations:
- No captures (`()` groups) — `string.match` returns the full match, not captures
- No frontier anchor `%f`
- No balanced match `%b`
- Character class sets `[...]` are not supported (treated as literals)

### 10. table.sort comparator not supported

`table.sort(t, comp)` ignores the comparator function and always sorts
using Lua's default `<` comparison. To use a custom comparator, write
a manual sort.

### 11. io library limited

- `io.open` returns a file object, but file methods (`file:read`,
  `file:write`, `file:lines`, `file:seek`) are not implemented.
- `io.read` only reads a single line from stdin.
- `io.lines` is not implemented (needs coroutines).

### 12. No metatables

`setmetatable` and `getmetatable` are implemented (they store a
metatable pointer on the table), but metamethods (`__index`, `__newindex`,
`__add`, `__sub`, `__eq`, `__lt`, `__le`, `__call`, `__tostring`, etc.)
are NOT dispatched. This means OOP patterns don't work.

### 13. No debug library

The `debug` library is not implemented.

## Performance limitations

### 14. Function bodies use table lookups, not VORTEX locals

Function bodies use the scope-table model: all variable accesses are
table field lookups (`SCOPE_GET`/`SCOPE_SET`), not VORTEX locals. This
means the VORTEX JIT can't keep function-local variables in registers.
The main chunk DOES use VORTEX locals (fast path).

**Impact**: Function bodies are slower than they could be. The VORTEX
JIT can still optimize the bytecode dispatch loop and the table lookup
helpers (via inline caches), but not as well as direct locals.

**Fix**: Implement proper upvalue analysis + cell allocation so function
bodies can use VORTEX locals for non-captured variables.

### 15. No GC integration

Lua heap objects (strings, tables, functions) are allocated with
`malloc` and never freed. VORTEX's generational GC doesn't know about
them. Long-running programs will leak memory.

**Fix**: Integrate with VORTEX's GC by registering Lua heap objects
as GC roots and implementing trace functions for each object kind.

## Parser limitations

### 16. Dotted function names not fully supported

`function a.b.c() ... end` is parsed but only the first name (`a`) is
used. The dotted path is ignored. Use `a.b.c = function() ... end`
instead.

### 17. Method definition syntax

`function obj:method() ... end` is not supported. Use
`obj.method = function(self) ... end` instead.

## Build issues

### 18. CMake 3.25+ required by VORTEX

The vendored VORTEX CMakeLists.txt requires CMake 3.25. If your system
has an older CMake, use the `build.sh` script instead (which doesn't
use CMake).

### 19. GCC computed-goto required

VORTEX's interpreter uses GCC's labels-as-values extension. This
requires GCC or Clang (not MSVC). The `VTX_USE_COMPUTED_GOTO` flag
must be enabled.
