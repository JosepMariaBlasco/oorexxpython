"""t80: the conversion probe prints what it is expected to print.

tests/probe/edges.rex shows how values cross the bridge both ways (Rexx
strings that look like numbers, .true/.false and .Boolean, .nil, Arrays,
other Rexx objects, bytes; Python's bool, None, numbers, bytes, str,
containers, numpy scalars).  Its expected output is tests/probe/edges.out:
a conversion that changes makes this test fail, so a change in the rules
is always a deliberate one, made in the same commit as the new expected
output.  The ooRexx bridges for .NET, Lua and JavaScript run probes of
their own, and the family compares them.

Run by tests/run.sh, as the other Python tests: the probe runs with the
`rexx` on the PATH (ORX_PYTHON, REXX_PATH and the library path as run.sh
sets them), in its own process.  The output is compared byte for byte, line
by line (line ends aside).
"""
import os, subprocess, sys

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "probe")
probe = os.path.join(here, "edges.rex")
with open(os.path.join(here, "edges.out"), "rb") as f:
    expected = f.read().replace(b"\r\n", b"\n").split(b"\n")

env = dict(os.environ)
env.setdefault("ORX_PYTHON", sys.executable)
try:
    run = subprocess.run(["rexx", probe], capture_output=True, timeout=120, env=env,
                         cwd=os.environ.get("TMPDIR", os.environ.get("TEMP", None)))
    got = (run.stdout + run.stderr).replace(b"\r\n", b"\n").split(b"\n")
except subprocess.TimeoutExpired:
    got = [b"(timed out)"]

checks = failures = 0
for i in range(max(len(expected), len(got))):
    e = expected[i] if i < len(expected) else b"(no line)"
    g = got[i] if i < len(got) else b"(no line)"
    checks += 1
    label = e[:45].decode("utf-8", "replace").strip() or "(empty line)"
    if g == e:
        print(f"ok {checks} - {label}")
    else:
        failures += 1
        print(f"not ok {checks} - line {i + 1}: {label}\n   got:      {g!r}\n   expected: {e!r}")

print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
