-- closures.lua — Lua closures and higher-order functions
local function counter()
    local n = 0
    return function()
        n = n + 1
        return n
    end
end

local c1 = counter()
local c2 = counter()
print("c1:", c1())  -- 1
print("c1:", c1())  -- 2
print("c1:", c1())  -- 3
print("c2:", c2())  -- 1 (independent)
print("c1:", c1())  -- 4

-- Map function
local function map(arr, f)
    local result = {}
    for i, v in ipairs(arr) do
        result[i] = f(v)
    end
    return result
end

local nums = {1, 2, 3, 4, 5}
local squared = map(nums, function(x) return x * x end)
print("--- squares ---")
for i, v in ipairs(squared) do
    print(i, v)
end

-- Filter
local function filter(arr, pred)
    local result = {}
    for _, v in ipairs(arr) do
        if pred(v) then
            table.insert(result, v)
        end
    end
    return result
end

local evens = filter(nums, function(x) return x % 2 == 0 end)
print("--- evens ---")
for _, v in ipairs(evens) do
    print(v)
end

-- Reduce
local function reduce(arr, f, init)
    local acc = init
    for _, v in ipairs(arr) do
        acc = f(acc, v)
    end
    return acc
end

local sum = reduce(nums, function(a, b) return a + b end, 0)
print("sum =", sum)
local product = reduce(nums, function(a, b) return a * b end, 1)
print("product =", product)
