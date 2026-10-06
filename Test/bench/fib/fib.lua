local n = 40
if arg and arg[1] then n = tonumber(arg[1]) end

local function fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end

print(fib(n))
