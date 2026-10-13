"""t50: Rexx classes that extend Python classes (.py~extend), used from
Python: instantiated and subclassed by Python, called from several Python
threads, their pairs released by the two collectors; Python exceptions
through Rexx come back as themselves."""
import gc, sys, threading
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
  return .py~extend(.Worker, "threading.Thread")
::requires "python.cls"
::class Worker subclass PyObject
::method init
  expose n
  use arg n
  self~init:super(.py~kw("name", "w"n))
::method run
  expose n
  self~setattr("out", n * n)
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
check("Python subclass, super()", (type(s).__name__, s.out), ("Sub", 26))

# an immutable base: INIT gives the value to str.__new__
Tag = r.routine('''
  return .py~extend(.Tag, "str")
::requires "python.cls"
::class Tag subclass PyObject
::method init
  use arg value
  self~init:super("<" || value || ">")
::method bare
  return self~strip("<>")
''')()
t = Tag("b")
check("str subclass made in Python", (t, isinstance(t, str), t.bare()), ("<b>", True, "b"))
check("made with __new__ alone, it has its Rexx object", Tag.__new__(Tag, "u").bare(), "u")
class Loud(Tag):
    def bare(self):
        return super().bare().upper()
check("Python subclass of it", (Loud("i"), Loud("i").bare()), ("<i>", "I"))

# keyword arguments: to the Rexx INIT (a .PyKwArgs), then to the base
Point = r.routine('''
  .py~exec("class PointBase:"                                   || "0a"x || -
           "    def __init__(self, x=0, y=0): self.x, self.y = x, y")
  return .py~extend(.Point, .py~main~PointBase)
::requires "python.cls"
::class Point subclass PyObject
::method sum
  return self~x + self~y
''')()
check("keyword arguments to the base", Point(y=4, x=3).sum(), 7)

# several Python threads at once in Rexx: needs ooRexx 5.3.0
if r.eval(".RexxInfo~version~left(3)") >= 5.3:
    ts = [Worker(i) for i in range(8)]
    for t in ts: t.start()
    for t in ts: t.join()
    check("8 threads", [t.out for t in ts], [i * i for i in range(8)])

    # pairs moving back and forth between the two sides, from several
    # threads, while both collectors run: all of them released at the end
    Pet = r.routine('''
      .py~exec("class PetBase:"                                  || "0a"x || -
               "    def __init__(self, n): self.n = n"            || "0a"x || -
               "    def sound(self): return 'generic'"            || "0a"x || -
               "    def describe(self): return '%d says %s' % (self.n, self.sound())")
      .local~petsReleased = 0
      return .py~extend(.Pet, .py~main~PetBase)
    ::requires "python.cls"
    ::class Pet subclass PyObject
    ::method sound
      return "woof"
    ::method uninit
      .local~petsReleased += 1
    ''')()
    hold = r.routine('''
      use arg pets                      -- Rexx keeps some of them for a while
      if \\.local~hasEntry("PETSHELD") then .local~petsHeld = .array~new
      do p over pets
        .local~petsHeld~append(p)
      end
      if .local~petsHeld~items > 40 then .local~petsHeld = .array~new
      junk = .array~new
      do 3000; junk~append(.Object~new); end
      return pets~makeArray~items
    ''')
    errors = []
    def churn(k):
        try:
            for round in range(10):
                pets = [Pet(k * 1000 + round * 10 + i) for i in range(10)]
                hold(pets)
                for p in pets:
                    if not p.describe().endswith("says woof"):
                        errors.append(p.describe())
                del pets
                gc.collect()
        except Exception as e:
            errors.append(repr(e))
    ts = [threading.Thread(target=churn, args=(k,)) for k in range(4)]
    for t in ts: t.start()
    for t in ts: t.join()
    check("4 threads, 400 pairs back and forth: no errors", errors, [])
    released = r.routine('''
      .local~petsHeld = .array~new
      gc = .py~import("gc")
      do 8
        junk = .array~new
        do 30000; junk~append(.Object~new); end
        drop junk
        gc~collect
      end
      return .local~petsReleased
    ''')
    check("... and all of them released, once each", released(), 400)

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
