-- fib.lua — Fibonacci numbers
local function fib(n)
    if n < 2 then
        return n
    end
    return fib(n - 1) + fib(n - 2)
end

for i = 0, 20 do
    print("fib(" .. i .. ") =", fib(i))
end

-- Iterative version
local function fib_iter(n)
    local a, b = 0, 1
    for i = 1, n do
        a, b = b, a + b
    end
    return a
end
print("---")
print("fib_iter(30) =", fib_iter(30))
print("fib_iter(40) =", fib_iter(40))
print("fib_iter(50) =", fib_iter(50))
