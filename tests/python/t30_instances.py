"""t30: closing interpreters, and Rexx objects passed between interpreters.

All interpreter instances in a process share one object memory, so a Rexx
object made in one instance can be used in another (passed through Python, or
through .environment), even after its instance has been closed
(docs/design.md, *Lifetime*).  Closing an instance must still let go of the
Python objects its Rexx objects held, without freeing those another instance
can still reach.
"""
import gc, sys, weakref
import oorexx

checks = failures = 0
def check(description, got, expected):
    global checks, failures
    checks += 1
    if got == expected:
        print(f"ok {checks} - {description}")
    else:
        failures += 1
        print(f"not ok {checks} - {description}\n   got:      {got!r}\n   expected: {expected!r}")

def raises(f):
    try:
        f()
    except Exception as e:
        return f"{type(e).__name__}: {e}"
    return "no exception"

class Thing:
    def __init__(self, n): self.n = n
    def hello(self): return f"hello {self.n}"

CHURN = "do 20000; x = .array~new(200); end; return 0"   # forces collections

def settle(r):
    """collections (uninits run), then a call (the bridge drops what they queued)"""
    churn = r.routine(CHURN)
    for _ in range(3):
        churn()
        gc.collect()

# 1. A Python object that a closed instance's Rexx code still held is released
probe = Thing(0)
base = sys.getrefcount(probe)
for i in range(20):
    r = oorexx.Interpreter()
    r.routine("use arg o; .local~kept = o; return 0")(probe)
    r.close()
other = oorexx.Interpreter()
settle(other)
check("closing 20 interpreters leaks no reference", sys.getrefcount(probe) - base, 0)

# 2. A Rexx object of A, holding a Python object, used by B after A is closed
a = oorexx.Interpreter()
b = oorexx.Interpreter()
t = Thing(1)
alive = weakref.ref(t)
d = a.routine("use arg t; d = .directory~new; d~t = t; return d")(t)
b.routine("use arg d; .local~kept = d; return 0")(d)
check("a .PyObject of A comes back from B as itself",
      b.routine("use arg d; return d~t")(d) is t, True)
del d, t
a.close()
settle(b)
check("B still reaches it after A is closed", alive() is not None, True)
check("B can use it", b.routine("return .local~kept~t~hello")(), "hello 1")
b.routine(".local~kept = .nil; return 0")()
settle(b)
check("released once B drops it", alive() is None, True)

# 3. A proxy of a closed instance's object cannot be passed to another instance
a = oorexx.Interpreter()
d = a.eval(".directory~new")
a.close()
check("closed instance's object into another instance",
      "has been closed" in raises(lambda: b.routine("use arg d; return d~class~id")(d)), True)
check("the other instance still works", b.eval("6 * 7"), 42)

# 4. .environment is shared by every instance
a = oorexx.Interpreter()
a.routine(".environment~orxpy.t30 = .py~list(.array~of(1, 2)); return 0")()
check(".environment crosses instances", b.eval(".environment~orxpy.t30"), [1, 2])
a.close()
b.routine(".environment~remove('ORXPY.T30'); return 0")()
check(".environment entry still usable until removed", b.eval("6 * 7"), 42)

# 5. A new interpreter is usable even where ooRexx reuses a closed one's address
error = None
for i in range(40):
    try:
        r = oorexx.Interpreter()
        r.routine("use arg o; x = o; return 0")(Thing(i))
        r.close()
        r = oorexx.Interpreter()
        r.routine(CHURN)()
        r.close()
    except Exception as e:
        error = f"cycle {i}: {e}"
        break
check("40 open/close cycles", error, None)

b.close()
other.close()
print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
