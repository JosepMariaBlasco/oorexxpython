"""t35: ADDRESS environments registered and resolved while Rexx runs on other threads.

Python registers environments (add_command_environment) while Python threads
run Rexx in the same interpreter, and those threads address environments
that may not be registered yet.  Before ooRexx r13274 registering changed the
handler table without the interpreter lock (bug #2111), and before r13275
resolving an unknown environment wrote into an unanchored object with the
kernel lock released (bug #2112): a crash, a hang or a lost registration,
now and then.  Skipped on an older ooRexx (5.2.0 included), where it would
fail in that way.

Run by tests/run.sh, as the other Python tests.  THREADS, ROUNDS and NAMES
come from ORXPY_ADDRESS_THREADS ("8 300 200" by default).
"""
import os, sys, threading
import oorexx

FIXED_IN = 13275  # bugs #2111 (r13274) and #2112 (r13275)

checks = failures = 0
def check(description, got, expected):
    global checks, failures
    checks += 1
    if got == expected:
        print(f"ok {checks} - {description}")
    else:
        failures += 1
        print(f"not ok {checks} - {description}\n   got:      {got!r}\n   expected: {expected!r}")

THREADS, ROUNDS, NAMES = (int(x) for x in
                          os.environ.get("ORXPY_ADDRESS_THREADS", "8 300 200").split())

r = oorexx.Interpreter(command_environments={"ECHO": lambda command, ctx: len(command)})
revision = int(str(r.routine("return .RexxInfo~revision")()))
if revision < FIXED_IN:
    print(f"ok - environments registered while Rexx runs # SKIP needs ooRexx r{FIXED_IN} (this is r{revision})")
    print(f"0 checks, 0 failed (skipped: needs ooRexx r{FIXED_IN}, this is r{revision})")
    r.close()
    sys.exit(0)

# One call: a command to an environment that is always there, then one to
# E<n>, registered or not yet.  RC is n + 1 once registered; before, ooRexx's
# own RC for an unknown environment (a FAILURE, not traced here).
call = r.routine("""
use arg n
trace off
address echo 'abc'
if rc <> 3 then return 'ECHO gave' rc
address value 'E'n
'x'
return rc
""")
unknown = int(str(call(-1)))      # E-1 is never registered

bad, errors = [], []
calls = [0] * THREADS
registering = threading.Event()
registering.set()

def worker(k):
    try:
        i = 0
        while registering.is_set() or i < ROUNDS:
            n = i % NAMES
            rc = str(call(n))
            if rc not in (str(n + 1), str(unknown)):
                bad.append((n, rc))
            calls[k] += 1
            i += 1
    except Exception as e:
        errors.append(repr(e))

threads = [threading.Thread(target=worker, args=(k,)) for k in range(THREADS)]
for t in threads: t.start()
for n in range(NAMES):
    r.add_command_environment(f"E{n}", lambda command, ctx, n=n: n + 1)
registering.clear()
for t in threads: t.join(120)

check("every thread finished", [t.is_alive() for t in threads].count(True), 0)
check("no exceptions", errors[:3], [])
check("every RC was the environment's or ooRexx's for an unknown one", bad[:3], [])
check("every thread ran its rounds", all(c >= ROUNDS for c in calls), True)
check("every environment registered",
      [n for n in range(NAMES) if str(call(n)) != str(n + 1)], [])
check("an unknown environment still fails", int(str(call(-1))), unknown)
r.close()
print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
