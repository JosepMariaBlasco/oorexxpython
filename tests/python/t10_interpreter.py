"""t10: Python -> Rexx: oorexx.Interpreter, Rexx objects, stdio, ADDRESS, threads."""
import contextlib, io, os, sys, tempfile, threading
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

tmp = tempfile.mkdtemp()
with open(os.path.join(tmp, "hello.rex"), "w") as f:
    f.write("use arg who\nreturn 'hello' who\n")
with open(os.path.join(tmp, "lib.cls"), "w") as f:
    f.write("::routine twice public\n  return arg(1) * 2\n")

r = oorexx.Interpreter(rexx_path=[tmp])
check("eval (numbers)",           r.eval("6 * 7"), 42)
check("eval string",           r.eval("'abc'~reverse"), "cba")
times = r.routine("use arg a, b; return a * b")
check("routine is callable",   times(6, 7), 42)
check("run program + args",    r.run(os.path.join(tmp, "hello.rex"), "world"), "hello world")
check("rexx_path search",      r.run("hello", "path"), "hello path")
check("::requires via path",   r.routine("return twice(21)\n::requires 'lib.cls'")(), 42)

# Rexx objects in Python
d = r.eval(".directory~new")
check("type",                  type(d).__name__, "RexxObject")
d["X"] = 1
d["Y"] = "two"
check("o[k] = v / o[k]",       d["Y"], "two")
check("message",               d.items(), 2)
check("len",                   len(d), 2)
check("iteration",             sorted(r.eval(".array~of('b', 'a')")), ["a", "b"])
check("Array -> list",         r.eval(".array~of(1, 'a')"), [1, "a"])
check("str",                   str(r.eval(".directory~new")), "a Directory")

# Python objects in Rexx
check("str arg",               r.routine("use arg s; return s~upper")("abc"), "ABC")
check("list is a .PyObject",   r.routine("use arg l; return l~class~id")([1, 2, 3]), "PYOBJECT")
check(".py~len(list)",         r.routine("use arg l; return .py~len(l)")([1, 2, 3]), 3)
check("same object back",      r.routine("use arg o; return o")(d) is not None, True)
lst = [1, 2]
check("identity Python obj",   r.routine("use arg o; return o")(lst) is lst, True)
check("Python callback",       r.routine("use arg f; return f~new(20) + 1")(lambda x: x * 2), 41)

# errors
check("Rexx error",            raises(lambda: r.eval("1 / 0")).startswith("RexxError: Rexx error 42.3"), True)
check("syntax error",          raises(lambda: r.routine("x = (")).startswith("RexxError: Rexx error 35.1"), True)

# SAY goes through Python's sys.stdout (stdio=True)
buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    r.routine("say 'hi from Rexx'; say 'line 2'")()
check("say -> sys.stdout",     buf.getvalue(), "hi from Rexx\nline 2\n")
old = sys.stdin
sys.stdin = io.StringIO("typed line\n")
check("pull <- sys.stdin",     r.routine("parse pull x; return x")(), "typed line")
sys.stdin = old

# ADDRESS written in Python
def calc(command, ctx):
    if command == "lines":
        for line in ctx.lines():
            ctx.output(str(eval(line)))
        return 0
    if command == "setvar":
        ctx.set_var("answer", 42)
        return ctx.get_var("question")
    if command == "fail":
        return ctx.raise_error(7)
    if command == "boom":
        raise ValueError("bad command")
    return eval(command)
r.add_command_environment("CALC", calc)
check("RC from handler",       r.routine("address calc '2+3'; return rc")(), 5)
check("WITH INPUT/OUTPUT stems", r.routine(
    "in.1 = '1+1'; in.2 = '2*3'; in.0 = 2\n"
    "address calc 'lines' with input stem in. output stem out.\n"
    "return out.0 out.1 out.2")(), "2 2 6")
check("caller's variables",    r.routine(
    "question = 'what?'; address calc 'setvar'; return rc answer")(), "what? 42")
check("raise_error -> ERROR",  r.routine(
    "signal on error; address calc 'fail'; return 'no'\nerror: return 'ERROR' rc")(), "ERROR 7")
check("Python exception -> SYNTAX", raises(lambda: r.routine("address calc 'boom'")()).find("ValueError: bad command") > 0, True)

# threads: Python threads calling Rexx concurrently
square = r.routine("return arg(1) ** 2")
results = []
def worker():
    acc = 0
    for i in range(500):
        acc += int(square(i))
    results.append(acc)
ts = [threading.Thread(target=worker) for _ in range(8)]
for t in ts: t.start()
for t in ts: t.join(60)
check("threads",               results, [sum(i * i for i in range(500))] * 8)

# options
r2 = oorexx.Interpreter(initial_address="CALC2", command_environments={"CALC2": lambda c, ctx: len(c)})
check("initial_address + environments", r2.routine("'abcd'; return rc")(), 4)
r2.close()
check("closed",                raises(lambda: r2.eval("1")), "RexxError: the Rexx interpreter instance has been closed")
with oorexx.Interpreter() as r3:
    check("context manager",   r3.eval("2 + 2"), 4)
# close from another thread than the creator's
r4 = oorexx.Interpreter()
check("before close",          r4.eval("1 + 1"), 2)
t = threading.Thread(target=r4.close); t.start(); t.join(20)
check("closed from another thread", t.is_alive(), False)
r.close()
print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
