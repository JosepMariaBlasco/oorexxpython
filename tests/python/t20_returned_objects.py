"""t20: a Python object that Rexx returns to Python survives until Python owns it.

Regression test for a use-after-free: the object came back borrowed from its
.PyObject proxy, which only the attached thread's local references kept alive; after DetachThread a collection could
run the proxy's uninit and another thread's drainGraveyard could free the
object before the returning thread took the GIL back (intoRexx in bridge.cpp).
Threads that keep the GIL busy widen that window; with PYTHONMALLOC=debug the
freed object is poisoned, so the old bridge crashed in about a second.
"""
import os, subprocess, sys, threading

if os.environ.get("PYTHONMALLOC") != "debug":
    # rerun under Python's debug allocator, which makes a use-after-free crash
    env = dict(os.environ, PYTHONMALLOC="debug")
    sys.exit(subprocess.call([sys.executable, "-u", __file__] + sys.argv[1:], env=env))

import oorexx

N, ITER, HOGS = 8, 150, 2
r = oorexx.Interpreter()
make = r.routine("return .py~list(.array~of(1, 2, 3))")   # only its proxy owns it
churn = r.routine("do 20000; x = .array~new(200); end; return .py~len(.array~of(1))")

bad = []
def maker():
    for _ in range(ITER):
        o = make()
        if type(o) is not list or o != [1, 2, 3]:
            bad.append(repr(type(o)))
            return

def hog(stop):                            # keeps the GIL busy
    while not stop.is_set():
        sum(range(20000))

def churner(stop):                        # collections, uninits, graveyard drains
    while not stop.is_set():
        churn()

stop = threading.Event()
helpers = [threading.Thread(target=churner, args=(stop,)) for _ in range(N)]
helpers += [threading.Thread(target=hog, args=(stop,)) for _ in range(HOGS)]
makers = [threading.Thread(target=maker) for _ in range(N)]
for t in helpers + makers: t.start()
for t in makers: t.join()
stop.set()
for t in helpers: t.join()

if bad:
    print(f"not ok 1 - returned objects intact ({len(bad)} bad: {bad[:3]})")
    print("1 checks, 1 failed")
    sys.exit(1)
print(f"ok 1 - returned objects intact ({N} threads x {ITER} calls, {HOGS} GIL hogs)")
print("1 checks, 0 failed")
