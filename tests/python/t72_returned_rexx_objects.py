"""t72: Rexx objects that Python code gives back to Rexx, under load.

A Rexx object held only by its Python proxy stays alive through the proxy's
entry in the instance's keep Directory.  When Python code returns it to Rexx
(or lets a Rexx condition through), the proxy is let go once the value has
been converted, and its release is applied by the next thread that enters
the instance: the object must be held by the Rexx side before that, or a
collection can take it while the caller is about to use it (a crash).
Sixteen Rexx threads run calls into Python that return a fresh Rexx object or
let a Rexx condition through, while two Python threads make cycles of
proxies and force collections (docs/design.md, *Lifetime*).
"""
import gc, os, threading
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

N, T = (int(x) for x in os.environ.get("ORXPY_RETURNED", "2000 16").split())
r = oorexx.Interpreter()
fails = r.routine("raise syntax 40.1 array ('x')")
make = r.routine("return .directory~new")
def through(): fails()            # a Rexx condition let through Python
def fresh(): return make()        # a fresh proxy, the result its only reference
drive = r.routine("""
use arg through, fresh, n
bad = 0
do n
  bad += check(through)
  d = fresh~new
  if \\d~isA(.directory) then bad += 1
end
return bad
check: procedure
  use arg f
  signal on syntax name s
  f~new
  return 1
s: return condition('O')~code \\== '40.1'
""")
stop = False
def collector():
    junk = []
    while not stop:
        a = [make()]; a.append(a); junk.append(a)     # cycles holding proxies
        if len(junk) > 20: junk.clear(); gc.collect()
bad = []; lock = threading.Lock()
def worker():
    b = int(drive(through, fresh, N))
    with lock: bad.append(b)
cs = [threading.Thread(target=collector) for _ in range(2)]
ws = [threading.Thread(target=worker) for _ in range(T)]
for t in cs + ws: t.start()
for t in ws: t.join()
stop = True
for t in cs: t.join()
check(f"{N * T} returns and conditions, no object lost", sum(bad), 0)
r.close()
print(f"{checks} checks, {failures} failed")
raise SystemExit(1 if failures else 0)
