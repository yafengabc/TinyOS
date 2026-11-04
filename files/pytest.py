# pytest.py - the interpreter's own test suite.
#
# Every line is a claim about what the interpreter does.  Run it with:
#
#     py pytest.py
#
# It prints one line per check and a summary at the end.  A failure does
# not stop the script: the point is to see every claim that broke, not
# just the first.
#
# The suite is in two files, because a script this size costs about 70 KiB
# of the interpreter's arena and the ESP32-C3's task window cannot hold
# two of them at once.  This one has the language and the containers;
# pytest2.py has the functions and the builtins.

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

# ---- arithmetic ------------------------------------------------------
check("add", 2 + 3, 5)
check("sub", 10 - 4, 6)
check("mul", 6 * 7, 42)
check("div is integer", 7 / 2, 3)
check("mod", 7 % 2, 1)
check("mod follows divisor", -7 % 2, 1)
check("neg", -5, -5)
check("precedence", 2 + 3 * 4, 14)
check("parens", (2 + 3) * 4, 20)
check("compare", 1 < 2, True)
check("bool is int", True == 1, True)
check("not", not 0, True)
check("and short circuits", 0 and 1, 0)
check("or short circuits", 1 or 0, 1)

# ---- strings ---------------------------------------------------------
s = "TinyOS"
check("len", len(s), 6)
check("index", s[0], "T")
check("negative index", s[-1], "S")
check("concat", s + "!", "TinyOS!")
check("repeat", "ab" * 3, "ababab")
check("in", "iny" in s, True)
check("not in", "zzz" in s, False)
check("upper", s.upper(), "TINYOS")
check("lower", "ABC".lower(), "abc")
check("strip", "  pad  ".strip(), "pad")
check("find", s.find("OS"), 4)
check("find missing", s.find("zz"), -1)
check("replace", s.replace("Tiny", "Small"), "SmallOS")
check("split", "a,b,c".split(","), ["a", "b", "c"])
check("split default", "a b c".split(), ["a", "b", "c"])
check("join", "-".join(["x", "y"]), "x-y")

# ---- lists -----------------------------------------------------------
nums = [1, 2, 3]
check("list literal", nums, [1, 2, 3])
check("len", len(nums), 3)
nums.append(4)
check("append", nums, [1, 2, 3, 4])
nums.insert(0, 0)
check("insert", nums, [0, 1, 2, 3, 4])
check("pop", nums.pop(), 4)
check("pop left the rest", nums, [0, 1, 2, 3])
check("index", nums.index(2), 2)
check("in list", 2 in nums, True)
check("not in list", 9 in nums, False)
nums.remove(2)
check("remove", nums, [0, 1, 3])
check("negative index", nums[-1], 3)
check("list add", [1] + [2], [1, 2])
check("list multiply", [0] * 3, [0, 0, 0])

# ---- dicts -----------------------------------------------------------
d = {"name": "Ada", "age": 36}
check("len", len(d), 2)
check("get", d["name"], "Ada")
check("get missing key", d.get("nope"), None)
check("get default", d.get("nope", "none"), "none")
check("in dict", "name" in d, True)
d["city"] = "London"
check("assign", d["city"], "London")
check("keys", d.keys(), ["name", "age", "city"])
check("values", d.values(), ["Ada", 36, "London"])
check("items", d.items(), [["name", "Ada"], ["age", 36], ["city", "London"]])
del d["age"]
check("del", len(d), 2)
check("del removed the key", "age" in d, False)

print("----")
print("checks:", checks, "failed:", fails)
