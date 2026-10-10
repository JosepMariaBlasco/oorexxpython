"""check.py: is the ooRexx/Python bridge installed?  Python -> ooRexx.
    python check.py
Needs this folder on PYTHONPATH and ooRexx on the PATH (its library; on
Windows rexx.dll, also found through OOREXX_HOME).
"""
import os, sys
import oorexx

with oorexx.Interpreter() as rexx:
    version = rexx.eval(".rexxInfo~version 'r'.rexxInfo~revision")
    print(f"ooRexx/Python: Python {sys.version.split()[0]}, ooRexx {version}")
    got = rexx.routine("use arg a; return a~reverse")("Python")
    if got != "nohtyP":
        print(f"ooRexx/Python: FAILED (got {got!r})")
        sys.exit(1)

# Python always looks in the folder of the program it runs, so this program
# finds the bridge next to it even without PYTHONPATH; other programs (the
# examples, yours) would not.
def same(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))
bridge = os.path.dirname(os.path.abspath(oorexx.__file__))
here = os.path.dirname(os.path.abspath(__file__))
if same(bridge, here) and not any(same(p, bridge) for p in os.environ.get("PYTHONPATH", "").split(os.pathsep) if p):
    set_ = "set" if os.name == "nt" else "export"
    print("ooRexx/Python: NOT ON PYTHONPATH: found only because check.py is next to it;")
    print("ooRexx/Python: other programs (the examples) will not find it. Set PYTHONPATH:")
    print(f"    {set_} PYTHONPATH={bridge}")
    sys.exit(1)
print("ooRexx/Python: OK")
