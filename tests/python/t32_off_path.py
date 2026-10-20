"""t32: a Python program that starts ooRexx with ooRexx's folder off the PATH.

python.cls requires ooRexx's own logicalwrapper.cls, which ::requires looks
for in the current folder, REXX_PATH and the PATH: the bridge adds ooRexx's
.cls folder to every interpreter it creates.  On Windows, ooRexx also ends
the process in RexxCreateInterpreter when rexx.dll was loaded by its full
path (here: through OOREXX_HOME) and its folder is not on the PATH: the
bridge puts the folder there.  Run as a child process: ooRexx's folder taken
off the PATH, no REXX_PATH, from an empty working folder.
"""
import os, shutil, subprocess, sys, tempfile

checks = failures = 0
def check(description, got, expected):
    global checks, failures
    checks += 1
    if got == expected:
        print(f"ok {checks} - {description}")
    else:
        failures += 1
        print(f"not ok {checks} - {description}\n   got:      {got!r}\n   expected: {expected!r}")

rexx = shutil.which("rexx")
if rexx is None:
    print("skip: no rexx on the PATH to take off it")
    print("0 checks, 0 failed")
    sys.exit(0)
rexx_bin = os.path.dirname(os.path.realpath(rexx))
def is_oorexx(d):
    d = d.strip('"')
    return d and (os.path.exists(os.path.join(d, "rexx")) or os.path.exists(os.path.join(d, "rexx.exe"))
                  or os.path.exists(os.path.join(d, "logicalwrapper.cls")))
env = dict(os.environ)
env["PATH"] = os.pathsep.join(d for d in env.get("PATH", "").split(os.pathsep) if not is_oorexx(d))
env.pop("REXX_PATH", None)
if os.name == "nt":
    env["OOREXX_HOME"] = os.path.dirname(rexx_bin)   # rexx.dll by its full path
code = "\n".join([
    "import oorexx",
    "r = oorexx.Interpreter()",
    "print(r.routine('use arg t; return t~class~id')(True))",
    "print(r.routine('return .json~new~fromJSON(\"[true, 1]\")' + chr(10) + '::requires \"json.cls\"')())",
])
with tempfile.TemporaryDirectory() as empty:
    run = subprocess.run([sys.executable, "-c", code], cwd=empty, env=env,
                         capture_output=True, text=True, timeout=120)
check("ooRexx off the PATH: the interpreter starts (rc)", run.returncode, 0)
check("python.cls found logicalwrapper.cls", run.stdout.splitlines()[:1], ["LogicalWrapper"])
check("and json.cls, from Rexx code", run.stdout.splitlines()[1:2], ["[True, 1]"])
if run.returncode != 0:
    print("   stderr:", run.stderr.strip()[-600:])
print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
