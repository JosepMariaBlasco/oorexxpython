"""t90: a Python host keeps its signals once an Interpreter exists (Unix).

ooRexx's first RexxCreateInterpreter takes SIGINT, SIGTERM and SIGHUP from
the host; the bridge gives SIGTERM and SIGHUP back and puts its own SIGINT
handler in place, which halts Rexx while Rexx code runs for Python and is
Python's otherwise (docs/design.md, *Signals*).  Each case is a child
process: it says "ready", the parent sends the signal half a second later
and reads what the child printed.  When Rexx is the host, ooRexx owns the
signals, as before.
"""
import os, signal, subprocess, sys, time

checks = failures = 0
def check(description, got, expected):
    global checks, failures
    checks += 1
    if got == expected:
        print(f"ok {checks} - {description}")
    else:
        failures += 1
        print(f"not ok {checks} - {description}\n   got:      {got!r}\n   expected: {expected!r}")

if os.name == "nt":
    print("skip: Unix signals")
    print("0 checks, 0 failed")
    sys.exit(0)

CHILD = r'''
import os, signal, sys, threading, time
mode = sys.argv[1]
def caught():                    # the kernel's view, where /proc has it
    try:
        for l in open(f"/proc/{os.getpid()}/status"):
            if l.startswith("SigCgt"):
                m = int(l.split()[1], 16)
                return ",".join(n for n, s in (("HUP", 1), ("INT", 2), ("TERM", 15)) if m >> (s - 1) & 1)
    except OSError:
        pass
    return "?"
if mode.startswith("hup-"):
    signal.signal(signal.SIGHUP, lambda s, f: print("host's SIGHUP", flush=True))
import oorexx
before = caught()
r = oorexx.Interpreter()
if mode.startswith("second-"):
    r = oorexx.Interpreter()
print("caught:", before, "->", caught(), flush=True)
loop = r.routine("signal on halt; do 500; call SysSleep 0.01; end; return 'no HALT'\n"
                 "halt: return 'HALT'")
try:
    if mode.endswith("python"):
        print("ready", flush=True)
        for i in range(50): time.sleep(0.1)
        print("python slept to the end", flush=True)
    elif mode.endswith("rexx"):
        print("ready", flush=True)
        print("rexx:", loop(), flush=True)
    elif mode == "callback":     # Python code that Rexx calls
        def nap():
            try:
                for i in range(50): time.sleep(0.1)
                return "slept"
            except KeyboardInterrupt:
                return "KeyboardInterrupt in the callback"
        call = r.routine("use arg f; return f~invoke(.nil)")
        print("ready", flush=True)
        print("rexx:", call(nap), flush=True)
    elif mode == "thread":       # Rexx in another thread, the main one waits
        out = []
        t = threading.Thread(target=lambda: out.append(loop()))
        t.start()
        print("ready", flush=True)
        while t.is_alive(): t.join(0.05)
        print("rexx:", out[0], flush=True)
    if mode.startswith("hup-"):
        os.kill(os.getpid(), signal.SIGHUP)
        time.sleep(0.2)
except KeyboardInterrupt:
    print("KeyboardInterrupt", flush=True)
print("exits", flush=True)
'''
REXX_HOST = r'''
signal on halt
.py~exec("import time" || "0a"x || "def nap():" || "0a"x ||,
         "    for i in range(20): time.sleep(0.1)" || "0a"x || "    return 'python slept to the end'")
say "ready"
say .py~main~nap
call SysSleep 0.5
say "no HALT"
exit
halt: say "HALT in Rexx"
::requires "python.cls"
'''

here = os.path.dirname(os.path.abspath(__file__))
work = os.environ.get("TMPDIR", "/tmp")
child = os.path.join(work, f"t90_child_{os.getpid()}.py")
rexx_host = os.path.join(work, f"t90_rexx_host_{os.getpid()}.rex")
with open(child, "w") as f: f.write(CHILD)
with open(rexx_host, "w") as f: f.write(REXX_HOST)

def run(cmd, sig):
    """the child's output lines after "ready", and its return code"""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    head = []
    for line in p.stdout:
        head.append(line.rstrip())
        if line.strip() == "ready": break
    time.sleep(0.5)
    p.send_signal(sig)
    try:
        rest, _ = p.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        rest, _ = p.communicate()
        return head, ["(hung, killed)"], None
    return head, [l for l in rest.splitlines() if l], p.returncode

def py(mode, sig):
    return run([sys.executable, child, mode], sig)

try:
    head, out, rc = py("python", signal.SIGINT)
    if sys.platform.startswith("linux"):
        check("after Interpreter(), the kernel: only SIGINT caught (no HUP, TERM)",
              head[0], "caught: INT -> INT")
    check("SIGINT while Python runs: KeyboardInterrupt", (out, rc), (["KeyboardInterrupt", "exits"], 0))

    head, out, rc = py("rexx", signal.SIGINT)
    check("SIGINT while Rexx runs: HALT in Rexx (ooRexx's handler)",
          (out, rc), (["rexx: HALT", "exits"], 0))

    head, out, rc = py("callback", signal.SIGINT)
    check("SIGINT while Python code that Rexx calls runs: Python's",
          (out, rc), (["rexx: KeyboardInterrupt in the callback", "exits"], 0))

    head, out, rc = py("thread", signal.SIGINT)
    check("SIGINT while another thread runs Rexx: HALT there, nothing for the main thread",
          (out, rc), (["rexx: HALT", "exits"], 0))

    head, out, rc = py("second-python", signal.SIGINT)
    check("a second Interpreter(): SIGINT while Python runs is still Python's",
          (out, rc), (["KeyboardInterrupt", "exits"], 0))
    head, out, rc = py("second-rexx", signal.SIGINT)
    check("a second Interpreter(): SIGINT while Rexx runs is still a HALT",
          (out, rc), (["rexx: HALT", "exits"], 0))

    head, out, rc = py("python", signal.SIGTERM)
    check("SIGTERM while Python runs: the process ends", (out, rc), ([], -signal.SIGTERM))
    head, out, rc = py("rexx", signal.SIGTERM)
    check("SIGTERM while Rexx runs: the process ends", (out, rc), ([], -signal.SIGTERM))

    # with a SIGHUP of its own, ooRexx leaves the host's signals alone: no
    # ooRexx handler to halt with, so the bridge uses Halt()
    head, out, rc = py("hup-python", signal.SIGINT)
    check("host's SIGHUP: SIGINT while Python runs is Python's, SIGHUP still the host's",
          (out, rc), (["KeyboardInterrupt", "exits"], 0))
    head, out, rc = py("hup-rexx", signal.SIGINT)
    check("host's SIGHUP: SIGINT while Rexx runs is a HALT (Halt())",
          (out, rc), (["rexx: HALT", "host's SIGHUP", "exits"], 0))

    # Rexx as the host: ooRexx's signals, as before
    head, out, rc = run(["rexx", rexx_host], signal.SIGINT)
    check("Rexx host, SIGINT while Python runs: Python runs to its end, then HALT",
          (out, rc), (["python slept to the end", "HALT in Rexx"], 0))
finally:
    for f in (child, rexx_host):
        try: os.remove(f)
        except OSError: pass

print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
