import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 40


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


print(fib(n))
