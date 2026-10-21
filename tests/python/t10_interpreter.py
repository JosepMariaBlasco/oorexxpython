"""t10: Python -> Rexx: oorexx.Interpreter, Rexx objects, stdio, ADDRESS, closing.

Python threads calling one interpreter at once: t15_concurrent_calls.py.
"""
import contextlib, gc, io, os, sys, tempfile, threading, weakref
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

# booleans: Python's bool is ooRexx's .LogicalWrapper in Rexx, and back; Rexx
# code names the class after ::requires "logicalwrapper.cls" (it is not in
# .environment), and JSON's booleans (a subclass) come to Python as bool
LW = '\n::requires "logicalwrapper.cls"'
f = r.routine("use arg x; return x")
check("bool -> .LogicalWrapper", r.routine("use arg t, f; return t~class~id f~isA(.LogicalWrapper) (t + 1)" + LW)(True, False), "LogicalWrapper 1 2")
check("bool without ::requires", r.routine("use arg t; return t~class~id t~isA(t~class) (.LogicalWrapper~class~id)")(True), "LogicalWrapper 1 String")
check("bool round trip",       [f(True), f(False)], [True, False])
check(".LogicalWrapper -> bool", r.routine("return .LogicalWrapper~of(2 > 1)" + LW)(), True)
check("JSON's true -> bool",   r.routine('return .json~new~fromJSON("[true, false, 1]")\n::requires "json.cls"')(), [True, False, 1])
check("Rexx logical stays 1",  r.eval("1 = 1"), 1)

# errors
check("Rexx error",            raises(lambda: r.eval("1 / 0")).startswith("RexxError: Rexx error 42.3"), True)
check("syntax error",          raises(lambda: r.routine("x = (")).startswith("RexxError: Rexx error 35.1"), True)
def caught(f):
    try:
        f()
    except oorexx.RexxError as e:
        return e
err = caught(lambda: r.routine("say 'one'\nx = 1 / 0", name="DIVIDE")())
check("RexxError's attributes", (err.condition, err.code, err.rc, err.position),
      ("SYNTAX", "42.3", 42, 2))
check("RexxError's texts", (err.message, err.errortext),
      ("Arithmetic overflow; divisor must not be zero.", "Arithmetic overflow/underflow."))
check("RexxError's program", err.program, "DIVIDE")
check("RexxError's traceback", err.traceback.splitlines()[0], "     2 *-* x = 1 / 0")
err = caught(lambda: r.routine("raise syntax 93.900 array ('custom')")())
check("a raised condition's attributes", (err.code, err.rc, err.message), ("93.900", 93, "custom."))

# Rexx's standard streams go through Python's (stdio=True), with prefixes
buf, ebuf = io.StringIO(), io.StringIO()
with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(ebuf):
    r.routine("say 'hi from Rexx'; say 'line 2'")()
    r.routine(".output~lineout('a' || '0a'x || 'b'); call charout , 'c'; call charout , 'd' || '0a'x")()
    r.routine(".error~lineout('oops'); .traceOutput~lineout('t'); trace r; x = 1")()
check("say -> sys.stdout",     buf.getvalue(),
      "REXXout>hi from Rexx\nREXXout>line 2\nREXXout>a\nREXXout>b\nREXXout>cd\n")
check(".error, trace -> sys.stderr", ebuf.getvalue().splitlines()[:3],
      ["REXXerr>oops", "REXXtrc>t", "REXXtrc>     1 *-* x = 1"])
old = sys.stdin
sys.stdin = io.StringIO("typed line\nanother\n")
buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    pulled = r.routine("parse pull x; return x")()
    read = r.routine("return linein()")()
sys.stdin = old
check("pull <- sys.stdin",     pulled, "typed line")
check("linein <- sys.stdin",   read, "another")
check("input prompt prefix",   buf.getvalue(), "REXXin?>REXXin?>")
check("stream classes",        r.routine("return .output~current~class~id .input~current~class~id")(),
      "PYOUTPUTSTREAM PYINPUTSTREAM")
for kw, want in ((dict(prefixes=False), "x\n"), (dict(prefixes={"OUTPUT": "py> "}), "py> x\n")):
    with oorexx.Interpreter(**kw) as p:
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            p.routine("say 'x'")()
        check("prefixes=%r" % kw["prefixes"], buf.getvalue(), want)
with oorexx.Interpreter() as p:
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        p.routine(".output~current~prefix = .nil; say 'x'")()
    check("prefix = .nil",         buf.getvalue(), "x\n")
with oorexx.Interpreter(stdio=False) as p:
    check("stdio=False",           p.routine("return .output~current~class~id")(), "Stream")

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
check("Python exception -> SYNTAX", r.routine(
    "signal on syntax; address calc 'boom'; return 'no'\n"
    "syntax: c = condition('O'); return c~code .py~type(c~additional[2])~getattr('__name__')")(),
    "98.900 ValueError")
check("... and back as itself", raises(lambda: r.routine("address calc 'boom'")()).startswith("ValueError:"), True)
check("RC whatever its sign, no condition", r.routine(
    "signal on error; signal on failure; address calc '-3'; return rc\n"
    "error: failure: return condition('C')")(), -3)

# removing an environment: unknown again, as ooRexx's own (FAILURE, RC 30)
unknown_env = ("signal on failure; address calc '1'; return 'no'\n"
               "failure: return condition('C') rc")
check("remove_command_environment", r.remove_command_environment("calc"), None)
check("... then FAILURE RC 30",     r.routine(unknown_env)(), "FAILURE 30")
check("... removing again: ValueError", raises(lambda: r.remove_command_environment("CALC")),
      "ValueError: remove_command_environment: no ADDRESS environment written in Python named CALC")
check("... a name never registered", raises(lambda: r.remove_command_environment("nosuchenv")).startswith("ValueError"), True)
r.add_command_environment("CALC", calc)
check("... registered again",       r.routine("address calc '2+3'; return rc")(), 5)

# registering and removing from Rexx (.py~addCommandEnvironment)
check("from Rexx: registered", r.routine(
    ".py~addCommandEnvironment('LEN', .py~eval('lambda c, ctx: len(c)'))\n"
    "address len 'abcd'; return rc")(), 4)
check("... seen from Python's routines", r.routine("address len 'ab'; return rc")(), 2)
check("... removed from Python", r.remove_command_environment("len"), None)
check("... removed from Rexx",   r.routine(
    ".py~addCommandEnvironment('LEN', .py~eval('lambda c, ctx: len(c)'))\n"
    ".py~removeCommandEnvironment('len')\n"
    "signal on failure; address len 'x'; return 'no'\n"
    "failure: return condition('C') rc")(), "FAILURE 30")
check("... removing a missing name from Rexx: SYNTAX 93.900", r.routine(
    "signal on syntax; .py~removeCommandEnvironment('len'); return 'no'\n"
    "syntax: return 'SYNTAX' condition('O')~code")(), "SYNTAX 93.900")
check("... a Python function from Python, registered by Rexx", r.routine(
    "use arg f; .py~addCommandEnvironment('TWICE', f); address twice 'abc'; return rc")(
    lambda c, ctx: 2 * len(c)), 6)
for what, handler in ((".nil", ".nil"), ("a string", "'abc'")):
    check(f"... {what} as the handler: SYNTAX 93.900", r.routine(
        f"signal on syntax; .py~addCommandEnvironment('BAD', {handler}); return 'no'\n"
        "syntax: return 'SYNTAX' condition('O')~code")(),
        "SYNTAX 93.900")
check("... not callable from Python: TypeError", raises(lambda: r.add_command_environment("BAD", None)).startswith("TypeError"), True)
check("remove_command_environment(non-str)", raises(lambda: r.remove_command_environment(1)).startswith("TypeError"), True)

# a Rexx condition that goes through Python and back to Rexx is itself
raising = {
    "syntax": r.routine("raise syntax 40.1 array ('x')"),
    "halt":   r.routine("raise halt description 'h'"),
}
def raiser(command, ctx):
    raising[command]()
r.add_command_environment("RAISER", raiser)
trapper = """
use arg what
signal on syntax name trapped; signal on halt name trapped; signal on user x name trapped
address raiser what
return 'no condition'
trapped:
c = condition('O'); a = c~additional
if a~isA(.array) then a = a~makeString('l', ',')
return c~condition '|' c~code '|' a '|' c~description '|' c~result
"""
check("through a handler: SYNTAX itself", r.routine(trapper)("syntax"), "SYNTAX | 40.1 | x |  | The NIL object")
# a HALT is SYNTAX 4.1 when it leaves the Rexx code (ooRexx's, description
# gone), and a HALT again back in Rexx
check("... HALT: HALT again", r.routine(trapper)("halt"),
      "HALT | The NIL object | The NIL object |  | The NIL object")
check("... HALT, only SYNTAX trapped: SYNTAX 4.1", r.routine(
    "signal on syntax; address raiser 'halt'; return 'no'\n"
    "syntax: return 'SYNTAX' condition('O')~code condition('O')~additional[1]")(), "SYNTAX 4.1 HALT")
check("... HALT, untrapped: RexxError 4.1 in Python", raises(lambda: r.routine("address raiser 'halt'; return 'no'")()),
      "RexxError: Rexx error 4.1: Program interrupted with HALT condition.")
# as with a Ctrl-C, CALL ON's handler runs after the next clause (the nop)
check("... HALT, CALL ON HALT: the clause resumes, rc .nil", r.routine(
    "call on halt name h; trapped = 'no'; address raiser 'halt'; nop; return trapped '|' rc\n"
    "h: trapped = condition('C'); return")(), "HALT | The NIL object")
check("through a Python function: SYNTAX itself", r.routine(
    "use arg o; signal on syntax; o~go; return 'no'\n"
    "syntax: return 'SYNTAX' condition('O')~code condition('O')~additional[1]")(
    __import__("types").SimpleNamespace(go=lambda: raising["syntax"]())), "SYNTAX 40.1 x")
# any other condition keeps its description, ADDITIONAL and RESULT (one that
# reaches Python; ooRexx's API gives the host only SYNTAX from a Rexx call)
user_x = r.routine("d = .directory~new; d~condition = 'USER X'; d~description = 'the description'\n"
                   "d~additional = .array~of('extra'); d~result = 'the result'; return d")()
def user_raiser(command, ctx):
    e = oorexx.RexxError("Rexx condition USER X")
    e._rexx_condition = user_x
    raise e
r.add_command_environment("USERX", user_raiser)
check("... USER X with description, ADDITIONAL and RESULT", r.routine(trapper.replace("raiser", "userx"))("x"),
      "USER X | The NIL object | extra | the description | the result")

# a SIGNAL ON past Python: Rexx calls Python directly (a native method, an
# ADDRESS handler), Python calls Rexx code that does RAISE USER X ... RETURN.
# Every call from Python into Rexx takes its own thread context (intoRexx,
# AttachThread), so the condition never unwinds the Rexx caller through
# Python's frames: today (r13275) it is lost there and the RETURN value comes
# back; if ooRexx comes to deliver it, it must reach the caller as itself.
past_routine = r.routine('raise user x description "d" return 7')
past_object = r.routine("return .PastRaiser~new\n::class PastRaiser\n::method go\n"
                        "  raise user x description 'd' return 7")()
r.add_command_environment("PAST", lambda c, ctx: past_routine() if c == "routine" else past_object.go())
past = r.routine("""
use arg how, f
signal on user x name trapped
if how == 'function' then return 'result' f~go
address past f
return 'rc' rc
trapped:
return 'trapped' condition('C') condition('D')
""")
ns = __import__("types").SimpleNamespace
for label, how, f, value in (("function, routine", "function", ns(go=lambda: past_routine()), "result 7"),
                             ("function, method", "function", ns(go=lambda: past_object.go()), "result 7"),
                             ("handler, routine", "handler", "routine", "rc 7"),
                             ("handler, method", "handler", "method", "rc 7")):
    check(f"SIGNAL ON past Python ({label})", past(how, f) in (value, "trapped USER X d"), True)

# closing an interpreter releases its environments
def closing_handler(command, ctx): return 0
probe = weakref.ref(closing_handler)
with oorexx.Interpreter(command_environments={"GONE": closing_handler}) as rc_:
    rc_.add_command_environment("GONE2", closing_handler)
del closing_handler
gc.collect()
check("close releases the environments' handlers", probe() is None, True)

# options
r2 = oorexx.Interpreter(initial_address="CALC2", command_environments={"CALC2": lambda c, ctx: len(c)})
check("initial_address + environments", r2.routine("'abcd'; return rc")(), 4)
r2.close()
check("closed",                raises(lambda: r2.eval("1")), "RexxError: the Rexx interpreter instance has been closed")
check("no condition: None attributes", (lambda e: (e.condition, e.code, e.rc))(caught(lambda: r2.eval("1"))),
      (None, None, None))
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
