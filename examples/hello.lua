-- hello.lua — basic LuaVortex test
print("Hello, World!")
print("LuaVortex", "version", "0.1")
local x = 42
local y = 3.14
print("x =", x, "y =", y)
print("type(x) =", type(x))
print("type(y) =", type(y))
print("x + y =", x + y)
print("x * 2 =", x * 2)
print("x // 7 =", x // 7)
print("x % 7 =", x % 7)
print("x ^ 0.5 =", x ^ 0.5)
print("string.len('hello') =", string.len("hello"))
print("string.upper('hello') =", string.upper("hello"))
print(string.format("%d + %d = %d", 10, 20, 30))
