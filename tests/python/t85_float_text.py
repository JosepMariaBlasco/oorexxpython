"""t85: the text of a Python float in Rexx, checked against ooRexx itself.

A float that arrives in Rexx is written as ooRexx writes x + 0 under
NUMERIC DIGITS 17, on the float's shortest digits (Python's repr): plain
digits unless the integer part needs more than 17 places or the decimals
more than 34, else scientific with an uppercase E and a signed exponent
(1E+17, 1E-35).  A whole number written plain gets ".0", so that it goes
back to Python as a float (the number rule reads the point); -0.0 is kept;
NaN and the infinities are ooRexx's own nan, +infinity, -infinity
(docs/design.md, *Values*; the ooRexx bridges for .NET, Lua and JavaScript
write floats the same way, without the ".0").

For each value: the bridge's text, against ooRexx's x + 0 on the shortest
digits written as digits "E" exponent; and the text back to Python, by the
number rule, is the same float (the same bits).
"""
import math, random, struct, sys
import oorexx

checks = failures = 0
def check(description, got, expected, quiet=False):
    global checks, failures
    checks += 1
    if got == expected:
        if not quiet:
            print(f"ok {checks} - {description}")
    else:
        failures += 1
        print(f"not ok {checks} - {description}\n   got:      {got!r}\n   expected: {expected!r}")

def shortest(x):
    """digits and exponent of repr(x): x == int(digits) * 10**exp, no trailing zeros"""
    r = repr(abs(x))
    mant, _, e = r.partition("e")
    whole, _, frac = mant.partition(".")
    digits = (whole + frac).lstrip("0")
    exp = int(e or 0) - len(frac)
    stripped = digits.rstrip("0")
    return stripped, exp + len(digits) - len(stripped)

r = oorexx.Interpreter()
both = r.routine("use arg x, s; numeric digits 17; return '<' || x || '|' || (s + 0) || '>'")
back = r.routine("use arg x; return x")

# the decision's table
table = [(1.0, "1.0"), (1e16, "10000000000000000.0"), (1e17, "1E+17"), (1.5e17, "1.5E+17"),
         (1e21, "1E+21"), (1e-7, "0.0000001"), (1e-20, "0.00000000000000000001"),
         (1e-34, "0.0000000000000000000000000000000001"), (1e-35, "1E-35"),
         (1.2345678901234567e-10, "0.00000000012345678901234568"),   # its shortest digits end in 8
         (1.2345678901234567e-20, "1.2345678901234567E-20"),
         (sys.float_info.max, "1.7976931348623157E+308"), (5e-324, "5E-324"),
         (0.1 + 0.2, "0.30000000000000004"), (1 / 3, "0.3333333333333333"),
         (float(2**53 + 1), "9007199254740992.0"), (0.0, "0.0"), (-0.0, "-0.0"),
         (-2.5, "-2.5"), (-1e17, "-1E+17"), (123.0, "123.0"), (100.0, "100.0"),
         (float("nan"), "nan"), (float("inf"), "+infinity"), (float("-inf"), "-infinity")]
for x, text in table:
    check(f"{x!r} is {text}", both(x, "0")[1:].split("|")[0], text)

# against ooRexx: powers of ten, their neighbours, and random doubles of every magnitude
values = []
for k in range(-330, 310):
    p = float(f"1e{k}")
    values += [p, math.nextafter(p, math.inf), math.nextafter(p, 0), 1.5 * p, -p]
rng = random.Random(85)
while len(values) < 6000:
    x = struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]
    if math.isfinite(x):
        values.append(x)
values += [rng.uniform(-1e6, 1e6) for _ in range(1000)] + [float(rng.randint(-10**18, 10**18)) for _ in range(500)]

bad = 0
for x in values:
    if x == 0 or not math.isfinite(x):
        continue
    digits, exp = shortest(x)
    got, rexx = both(x, ("-" if x < 0 else "") + digits + "E" + str(exp))[1:-1].split("|")
    expected = rexx + (".0" if "E" not in rexx and "." not in rexx else "")
    if got != expected or struct.pack("<d", back(got)) != struct.pack("<d", x):
        bad += 1
        if bad <= 10:
            check(f"{x!r}", (got, back(got)), (expected, x))
check(f"{len(values)} doubles: the text is ooRexx's x + 0 under DIGITS 17, and reads back as the same float", bad, 0)

# the way back (Rexx string -> float): ooRexx's spellings, and Python's own
# (NaN, Infinity), which float() also reads
check("-0.0 back is -0.0", math.copysign(1, back(r.eval("'-0.0'"))), -1.0)
check("1E+17 back is a float", r.routine("return '1E+17'")(), 1e17)
for text, want in [("nan", "nan"), ("+infinity", "inf"), ("-infinity", "-inf"), ("NaN", "nan"), ("Infinity", "inf"), ("-Infinity", "-inf"), ("-0.0", "-0.0"),
                   ("1E+17", "1e+17"), ("0.0000001", "1e-07"), ("10000000000000000.0", "1e+16")]:
    check(f".py~float('{text}')", r.routine(f"return '<' || .py~repr(.py~float('{text}')) || '>'")(), f"<{want}>")
    check(f".py~box('float', '{text}')",
          r.routine(f"return '<' || .py~repr(.py~box('float', '{text}')) || '>'")(), f"<{want}>")

# explicit floats out of range: errors in Python's own types (overflow,
# underflow to zero); by name still works; a Python object keeps Python's
# semantics
def error(src):
    try:
        return repr(r.routine(src)())
    except (OverflowError, ValueError) as e:
        return f"{type(e).__name__}: {e}"
for form in (".py~float('{}')", ".py~box('float', '{}')"):
    for text, want in [("1E+999", "OverflowError: ooRexx/Python: 1E+999 is out of range for a float"),
                       ("-1.8E+308", "OverflowError: ooRexx/Python: -1.8E+308 is out of range for a float"),
                       ("1E-999", "ValueError: ooRexx/Python: 1E-999 is out of range for a float (it would be 0)"),
                       ("-1E-999", "ValueError: ooRexx/Python: -1E-999 is out of range for a float (it would be 0)"),
                       ("5E-324", "5e-324"), ("0E-999", "0.0"), ("+infinity", "inf"), ("nan", "nan")]:
        src = "return " + form.format(text)
        check(src, error(src), want)
check("box('complex', '1+1E-999j')", error("return .py~box('complex', '1+1E-999j')"),
      "ValueError: ooRexx/Python: 1+1E-999j is out of range for a complex (it would be 0)")
check(".py~float(a Decimal made in Python): Python's own float()",
      error("return .py~float(.py~eval(\"__import__('decimal').Decimal('1E999')\"))"), "inf")

print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
