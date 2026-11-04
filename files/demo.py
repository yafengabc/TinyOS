# demo.py - what the TinyOS Python interpreter can do.
#
# Run it with:  py demo.py

# ---- arithmetic and strings -------------------------------------------
x = 10
y = 3
print("sum:", x + y)
print("diff:", x - y)
print("prod:", x * y)
print("div:", x / y)
print("mod:", x % y)
print("neg:", -x)
print("cmp:", x > y, x == 10, x != y)

s = "TinyOS"
print("upper:", s.upper())
print("lower:", "ABC".lower())
print("find:", s.find("OS"))
print("replace:", s.replace("Tiny", "Small"))
print("split:", "a,b,c".split(","))
print("join:", "-".join(["x", "y", "z"]))
print("in:", "iny" in s)

# ---- lists -------------------------------------------------------------
nums = [1, 2, 3]
nums.append(4)
nums.insert(0, 0)
print("list:", nums)
print("len:", len(nums))
print("index of 3:", nums.index(3))
print("pop:", nums.pop(), "now:", nums)
print("3 in list:", 3 in nums)

# A list of lists, and a loop over it.
grid = [[1, 2], [3, 4]]
for row in grid:
    for cell in row:
        print("cell:", cell)

# ---- dicts -------------------------------------------------------------
person = {"name": "Ada", "age": 36}
person["city"] = "London"
print("name:", person["name"])
print("city:", person["city"])
print("keys:", person.keys())
print("values:", person.values())
print("items:", person.items())
print("get missing:", person.get("nope"))
print("get with default:", person.get("nope", "none"))
print("'name' in dict:", "name" in person)
print("len:", len(person))

del person["age"]
print("after del:", person)

# ---- control flow ------------------------------------------------------
if x > y:
    print("if: x is bigger")
elif x == y:
    print("elif: equal")
else:
    print("else: y is bigger")

i = 0
while i < 3:
    print("while:", i)
    i = i + 1

for n in range(4):
    if n == 1:
        continue
    if n == 3:
        break
    print("for:", n)

# ---- functions ---------------------------------------------------------
def add(a, b):
    return a + b

def greet(name, greeting):
    return greeting + ", " + name

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def counter():
    total = 0
    for k in range(5):
        total = total + k
    return total

print("add:", add(2, 3))
print("greet:", greet("Ada", "Hello"))
print("fib(10):", fib(10))
print("counter:", counter())

# A keyword argument.
print("greet kw:", greet(greeting="Hi", name="Bob"))

# ---- builtins ----------------------------------------------------------
nums = [3, 1, 4, 1, 5]
print("len:", len(nums))
print("sum:", sum(nums))
print("min:", min(nums))
print("max:", max(nums))
print("abs:", abs(-5))
print("str:", str(42))
print("int:", int("123"))
print("type:", type(nums), type(1), type("s"))
print("bool:", bool(0), bool(1))
print("range:", range(3))

print("done")
