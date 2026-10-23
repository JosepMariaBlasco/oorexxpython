"""t91: a Python host keeps Ctrl-C and Ctrl-Break (Windows).

Loading rexx.dll registers ooRexx's console control handler, which comes
before Python's: it took a Ctrl-C while Python ran (lost, or a crash before
the first Interpreter) and ignored Ctrl-Break.  The bridge's own handler
comes first now: a Ctrl-C while Rexx code runs for Python is a HALT in Rexx,
any other Ctrl-C is a KeyboardInterrupt, and Ctrl-Break does what Python
does with it (by default, the process ends) (docs/design.md, *Signals*).

A console event goes to every process attached to a console, so each case
runs in a console of its own: a driver process, started with a new console,
runs the child there, waits for its "ready", sends the event to the console
half a second later (surviving it itself) and prints what the child printed.
"""
import os, subprocess, sys, time

checks = failures = 0
def check(description, got, expected):
    global checks, failures
    checks += 1
    if got == expected:
        print(f"ok {checks} - {description}")
    else:
        failures += 1
        print(f"not ok {checks} - {description}\n   got:      {got!r}\n   expected: {expected!r}")

if os.name != "nt":
    print("skip: Windows console events")
    print("0 checks, 0 failed")
    sys.exit(0)

import ctypes
from ctypes import wintypes

CHILD = r'''
import ctypes, sys, threading, time
ctypes.WinDLL("kernel32").SetConsoleCtrlHandler(None, False)   # Ctrl-C on
mode = sys.argv[1]
try:
    if mode != "plain":
        import oorexx
    if mode not in ("plain", "import"):
        r = oorexx.Interpreter()
        loop = r.routine("signal on halt; do 500; call SysSleep 0.01; end; return 'no HALT'\n"
                         "halt: return 'HALT'")
    if mode in ("plain", "import", "python"):
        print("ready", flush=True)
        for i in range(50): time.sleep(0.1)
        print("python slept to the end", flush=True)
    elif mode == "rexx":
        print("ready", flush=True)
        print("rexx:", loop(), flush=True)
    elif mode == "callback":
        def nap():
            try:
                for i in range(50): time.sleep(0.1)
                return "slept"
            except KeyboardInterrupt:
                return "KeyboardInterrupt in the callback"
        call = r.routine("use arg f; return f~invoke(.nil)")
        print("ready", flush=True)
        print("rexx:", call(nap), flush=True)
    elif mode == "thread":
        out = []
        t = threading.Thread(target=lambda: out.append(loop()))
        t.start()
        print("ready", flush=True)
        while t.is_alive(): t.join(0.05)
        print("rexx:", out[0], flush=True)
except KeyboardInterrupt:
    print("KeyboardInterrupt", flush=True)
print("exits", flush=True)
'''

me = os.path.abspath(__file__)
if len(sys.argv) > 1 and sys.argv[1] == "driver":
    child, event, mode = sys.argv[2], int(sys.argv[3]), sys.argv[4]
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    HANDLER = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.DWORD)
    keep = HANDLER(lambda t: True)              # the driver survives the event
    k32.SetConsoleCtrlHandler(keep, True)
    p = subprocess.Popen([sys.executable, child, mode], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    for line in p.stdout:
        if line.strip() == "ready": break
    time.sleep(0.5)
    k32.GenerateConsoleCtrlEvent(event, 0)
    try:
        rest, _ = p.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        rest, _ = p.communicate()
        rest += "\n(hung, killed)"
    for l in rest.splitlines():
        if l: print(l)
    print(f"rc {p.returncode & 0xFFFFFFFF:#x}")
    sys.exit(0)

work = os.environ.get("TEMP", ".")
child = os.path.join(work, f"t91_child_{os.getpid()}.py")
output = os.path.join(work, f"t91_out_{os.getpid()}.txt")
with open(child, "w") as f: f.write(CHILD)

CTRL_C, CTRL_BREAK = 0, 1
ENDED = "0xc000013a"                            # STATUS_CONTROL_C_EXIT
def run(event, mode):
    """the child's lines after "ready", the last one its return code"""
    with open(output, "w") as f:
        d = subprocess.Popen([sys.executable, me, "driver", child, str(event), mode],
                             stdout=f, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NEW_CONSOLE)
        try:
            d.wait(90)
        except subprocess.TimeoutExpired:
            d.kill()
            return ["(driver hung)"]
    with open(output) as f:
        return [l.rstrip() for l in f if l.strip()]

try:
    check("no oorexx: Ctrl-C is a KeyboardInterrupt (the control)",
          run(CTRL_C, "plain"), ["KeyboardInterrupt", "exits", "rc 0x0"])
    check("import oorexx, no Interpreter yet: Ctrl-C is a KeyboardInterrupt",
          run(CTRL_C, "import"), ["KeyboardInterrupt", "exits", "rc 0x0"])
    check("after Interpreter(), Ctrl-C while Python runs: KeyboardInterrupt",
          run(CTRL_C, "python"), ["KeyboardInterrupt", "exits", "rc 0x0"])
    check("Ctrl-C while Rexx runs: HALT in Rexx",
          run(CTRL_C, "rexx"), ["rexx: HALT", "exits", "rc 0x0"])
    check("Ctrl-C while Python code that Rexx calls runs: Python's",
          run(CTRL_C, "callback"), ["rexx: KeyboardInterrupt in the callback", "exits", "rc 0x0"])
    check("Ctrl-C while another thread runs Rexx: HALT there, nothing for the main thread",
          run(CTRL_C, "thread"), ["rexx: HALT", "exits", "rc 0x0"])
    check("no oorexx: Ctrl-Break ends the process (the control)",
          run(CTRL_BREAK, "plain"), ["rc " + ENDED])
    check("import oorexx: Ctrl-Break ends the process",
          run(CTRL_BREAK, "import"), ["rc " + ENDED])
    check("after Interpreter(), Ctrl-Break while Python runs ends the process",
          run(CTRL_BREAK, "python"), ["rc " + ENDED])
    check("Ctrl-Break while Rexx runs ends the process",
          run(CTRL_BREAK, "rexx"), ["rc " + ENDED])
finally:
    for f in (child, output):
        try: os.remove(f)
        except OSError: pass

print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
