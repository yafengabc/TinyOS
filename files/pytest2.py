# pytest2.py - the functions and the builtins.
#
# Part of the interpreter's test suite; see pytest.py for the whole story.
# Split in two because a script of this size needs about 40 KiB of the
# interpreter's arena, and the ESP32-C3's task window cannot hold two of
# them at once.

fails = 0
checks = 0

def check(name, got, want):
    # No `global` statement: a name that is already bound is assigned
    # where it is bound, so `checks` and `fails` below are the ones at the
    # top of the file.
    checks = checks + 1
    if got == want:
        print("ok   ", name)
    else:
        fails = fails + 1
        print("FAIL ", name, "got", got, "want", want)

# ---- control flow ----------------------------------------------------
total = 0
for i in range(5):
    if i == 1:
        continue
    if i == 4:
        break
    total = total + i
check("for with continue and break", total, 5)

n = 0
while n < 3:
    n = n + 1
check("while", n, 3)

if 1 > 2:
    check("if", "no", "yes")
elif 2 > 1:
    check("elif", "yes", "yes")
else:
    check("else", "no", "yes")

# ---- functions -------------------------------------------------------
def add(a, b):
    return a + b

def fib(k):
    if k < 2:
        return k
    return fib(k - 1) + fib(k - 2)

def greet(who, greeting):
    return greeting + ", " + who

def counter():
    n = 0
    for k in range(4):
        n = n + k
    return n

check("call", add(2, 3), 5)
check("recursion", fib(10), 55)
check("string argument", greet("Ada", "Hi"), "Hi, Ada")
check("keyword argument", greet(greeting="Hi", who="Bob"), "Hi, Bob")
check("local scope", counter(), 6)

# ---- builtins --------------------------------------------------------
check("len list", len([1, 2]), 2)
check("len dict", len({"a": 1}), 1)
check("sum", sum([1, 2, 3]), 6)
check("min", min([3, 1, 2]), 1)
check("max", max([3, 1, 2]), 3)
check("abs", abs(-5), 5)
check("str", str(42), "42")
check("int", int("123"), 123)
check("type int", type(1), "int")
check("type list", type([]), "list")
check("bool", bool(0), False)
check("range", range(3), [0, 1, 2])
check("range two args", range(2, 5), [2, 3, 4])

print("----")
print("checks:", checks, "failed:", fails)
