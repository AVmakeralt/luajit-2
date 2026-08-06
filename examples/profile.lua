-- profile.lua — Performance profiling tool for LuaVortex
--
-- Breaks down where time is spent to identify bottlenecks.
-- Run: luavortex profile.lua

local function bench(name, fn, iterations)
    local s = os.clock()
    fn()
    local elapsed = os.clock() - s
    local per_iter = elapsed / iterations * 1e9  -- ns per iteration
    print(string.format("  %-30s %8.4fs  (%.0f ns/iter)", name, elapsed, per_iter))
    return elapsed
end

print("=== LuaVortex Performance Profiler ===")
print("")

local N = 1000000

-- 1. Pure arithmetic (no loops, no function calls)
bench("10 adds (no loop)", function()
    for i = 1, N do
        local x = i + i + i + i + i + i + i + i + i + i
    end
end, N * 10)

-- 2. Empty for loop (measures backward GOTO overhead)
bench("empty for loop (1M)", function()
    for i = 1, N do end
end, N)

-- 3. For loop with simple add
bench("for + add (1M)", function()
    local x = 0
    for i = 1, N do x = i + i end
end, N)

-- 4. For loop with local store
bench("for + store (1M)", function()
    local a = 0
    for i = 1, N do a = i end
end, N)

-- 5. Multiple assignment
bench("multi-assign a,b=b,a+b (1M)", function()
    local a, b = 0, 1
    for i = 1, N do a, b = b, a + b end
end, N)

-- 6. Empty function call (CALL_STATIC overhead)
bench("empty func call (1M)", function()
    local function noop() end
    for i = 1, N do noop() end
end, N)

-- 7. Function call with arg + return
bench("func call+ret (1M)", function()
    local function add1(x) return x + 1 end
    local r = 0
    for i = 1, N do r = add1(i) end
end, N)

-- 8. fib_iter(35) — the full benchmark
bench("fib_iter(35) (1M)", function()
    local function fib_iter(n)
        local a, b = 0, 1
        for i = 1, n do a, b = b, a + b end
        return a
    end
    local r = 0
    for i = 1, N do r = fib_iter(35) end
end, N)

-- 9. fib_iter(1) — just call overhead
bench("fib_iter(1) (1M)", function()
    local function fib_iter(n)
        local a, b = 0, 1
        for i = 1, n do a, b = b, a + b end
        return a
    end
    local r = 0
    for i = 1, N do r = fib_iter(1) end
end, N)

print("")
print("=== 35M iteration breakdown ===")
local M = 35000000

bench("35M empty loop", function()
    for i = 1, M do end
end, M)

bench("35M simple add", function()
    local a = 0
    for i = 1, M do a = i + i end
end, M)

bench("35M multi-assign", function()
    local a, b = 0, 1
    for i = 1, M do a, b = b, a + b end
end, M)

bench("35M unrolled adds", function()
    local a = 0
    for outer = 1, 1000000 do
        a = a+1; a = a+1; a = a+1; a = a+1; a = a+1
        a = a+1; a = a+1; a = a+1; a = a+1; a = a+1
        a = a+1; a = a+1; a = a+1; a = a+1; a = a+1
        a = a+1; a = a+1; a = a+1; a = a+1; a = a+1
        a = a+1; a = a+1; a = a+1; a = a+1; a = a+1
        a = a+1; a = a+1; a = a+1; a = a+1; a = a+1
        a = a+1; a = a+1; a = a+1; a = a+1; a = a+1
    end
end, M)

print("")
print("=== Analysis ===")
print("Each backward GOTO (loop iteration) calls:")
print("  1. vtx_profiler_record_backward_branch() → LRU lookup + increment")
print("  2. vtx_gc_safepoint() → check collection_requested flag")
print("  3. vtx_profiler_tier_up_check() → LRU lookup AGAIN + decrement")
print("")
print("Total per backward GOTO:")
print("  3 function calls (not inlined)")
print("  2× LRU lookups (8-entry scan + memmove on hit)")
print("  2× VTX_ASSERT checks")
print("  1× atomic load (compiled_code check)")
print("")
print("For comparison:")
print("  Lua 5.4:  ~14 ns/iter for fib_iter(35) = 0.5s total")
print("  LuaJIT:   ~2 ns/iter = 0.06s total")
