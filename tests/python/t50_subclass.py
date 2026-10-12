"""t50: Python classes with methods in Rexx (.py~extend) used from Python:
made by a Rexx routine, instantiated and subclassed by Python, called from
several Python threads; Python exceptions through Rexx come back as
themselves."""
import sys, threading
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

r = oorexx.Interpreter(prefixes=False)
Worker = r.routine('''
  return .py~extend("threading.Thread", .Worker)
::class Worker
::method __init__
  expose n
  use arg n, slot
  slot~super~__init__(.py~kw("name", "w"n))
::method run
  expose n
  use arg slot
  slot~self~setattr("out", n * n)
''')()

check("a Python class", isinstance(Worker, type) and issubclass(Worker, threading.Thread), True)
w = Worker(3)
w.start(); w.join()
check("run in Rexx", (w.out, w.name), (9, "w3"))

class Sub(Worker):                       # a Python subclass of it
    def run(self):
        super().run()
        self.out += 1
s = Sub(5); s.start(); s.join()
check("Python subclass, super()", s.out, 26)

# several Python threads at once in Rexx: needs ooRexx 5.3.0
if r.eval(".RexxInfo~version~left(3)") >= 5.3:
    ts = [Worker(i) for i in range(8)]
    for t in ts: t.start()
    for t in ts: t.join()
    check("8 threads", [t.out for t in ts], [i * i for i in range(8)])

# exceptions: raised in Python, through Rexx, back in Python as themselves
call = r.routine("use arg f; return f~new")
def bad():
    raise KeyError("missing")
try:
    call(bad)
    got = "no exception"
except KeyError as e:
    got = "KeyError " + str(e)
check("KeyError through Rexx", got, "KeyError 'missing'")
try:
    r.routine(".py~raise(.py~StopIteration)")()
    got = "no exception"
except StopIteration:
    got = "StopIteration"
check(".py~raise", got, "StopIteration")
try:
    r.eval("1 / 0")
    got = "no exception"
except oorexx.RexxError as e:
    got = str(e)[:20]
check("a Rexx error is still a RexxError", got, "Rexx error 42.3: Ari")

r.close()
print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
