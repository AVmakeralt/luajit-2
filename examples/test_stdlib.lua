-- test_stdlib.lua — comprehensive stdlib test
-- Note: mutable upvalues from the main chunk don't work yet (see BUGS.md).
-- We use a table to hold the counters so they're shared by reference.
local counts = {pass = 0, fail = 0}

local function check(name, got, expected)
    if type(got) == "number" and type(expected) == "number" then
        if got == expected or (got ~= got and expected ~= expected) or
           (math.abs(got - expected) < 1e-9) then
            counts.pass = counts.pass + 1
        else
            counts.fail = counts.fail + 1
            print("FAIL " .. name .. ": got " .. tostring(got) .. ", expected " .. tostring(expected))
        end
    elseif tostring(got) == tostring(expected) then
        counts.pass = counts.pass + 1
    else
        counts.fail = counts.fail + 1
        print("FAIL " .. name .. ": got " .. tostring(got) .. ", expected " .. tostring(expected))
    end
end

-- Math library
check("math.abs", math.abs(-5), 5)
check("math.floor", math.floor(3.7), 3)
check("math.ceil", math.ceil(3.2), 4)
check("math.sqrt", math.sqrt(16), 4)
check("math.sin", math.sin(0), 0)
check("math.cos", math.cos(0), 1)
check("math.pi", math.pi, 3.14159265358979)
check("math.max", math.max(1, 5, 3), 5)
check("math.min", math.min(1, 5, 3), 1)
check("math.type", math.type(42), "integer")
check("math.type(3.14)", math.type(3.14), "float")
check("math.tointeger", math.tointeger(3.0), 3)
check("math.asin", math.asin(0), 0)
check("math.acos", math.acos(1), 0)
check("math.tanh", math.tanh(0), 0)

-- String library
check("string.len", string.len("hello"), 5)
check("string.upper", string.upper("hello"), "HELLO")
check("string.lower", string.lower("HELLO"), "hello")
check("string.sub", string.sub("hello", 2, 4), "ell")
check("string.rep", string.rep("ab", 3), "ababab")
check("string.reverse", string.reverse("hello"), "olleh")
check("string.byte", string.byte("A"), 65)
check("string.char", string.char(65, 66), "AB")
check("string.format", string.format("%d + %d = %d", 2, 3, 5), "2 + 3 = 5")
check("string.find", string.find("hello world", "world"), 7)
check("string.match", string.match("hello 123 world", "%d+"), "123")
check("string.gsub", string.gsub("hello", "l", "L"), "heLLo")

-- Table library
local t = {3, 1, 2}
table.sort(t)
check("table.sort", t[1] .. t[2] .. t[3], "123")
check("table.concat", table.concat({1, 2, 3}, "-"), "1-2-3")
check("table.pack", table.pack(1, 2, 3).n, 3)

-- OS library
check("os.time", type(os.time()), "number")
check("os.clock", type(os.clock()), "number")
check("os.date", type(os.date()), "string")

-- Basic functions
check("type", type(42), "number")
check("tostring", tostring(42), "42")
check("tonumber", tonumber("42"), 42)
check("tonumber(hex", tonumber("ff", 16), 255)
check("rawget", rawget({a=1}, "a"), 1)
check("rawset", (function() local t = {}; rawset(t, "x", 5); return t.x end)(), 5)
check("rawequal", rawequal(1, 1), true)

-- Closures (all compiled now)
local function counter()
    local n = 0
    return function() n = n + 1; return n end
end
local c1 = counter()
local c2 = counter()
check("closure1", c1(), 1)
check("closure2", c1(), 2)
check("closure3", c2(), 1)  -- independent
check("closure4", c1(), 3)

-- Recursive function (compiled)
local function fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end
check("fib", fib(10), 55)

print("\n" .. counts.pass .. " passed, " .. counts.fail .. " failed")
if counts.fail > 0 then os.exit(1) end
