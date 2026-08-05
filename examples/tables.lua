-- tables.lua — Lua table operations
local t = {1, 2, 3, 4, 5}
print("Length:", #t)
print("Sum:", t[1] + t[2] + t[3] + t[4] + t[5])

-- Append
table.insert(t, 6)
print("After insert(6):", #t, "last =", t[#t])

-- Remove
local removed = table.remove(t, 1)
print("Removed:", removed, "first is now:", t[1])

-- Concat
print("Concat:", table.concat(t, ", "))

-- Hash table
local person = {
    name = "Alice",
    age = 30,
    city = "Springfield",
}
print(person.name, person.age, person.city)
person["email"] = "alice@example.com"
print(person.email)

-- Iterate
print("--- pairs ---")
for k, v in pairs(person) do
    print(k, "=", v)
end

-- Sort
local nums = {5, 3, 8, 1, 9, 2, 7}
table.sort(nums)
print("--- sorted ---")
for i, v in ipairs(nums) do
    print(i, v)
end
