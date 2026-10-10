"""check.py: is the ooRexx/Python bridge installed?  Python -> ooRexx.
    python check.py
Needs this folder on PYTHONPATH and ooRexx on the PATH (its library; on
Windows rexx.dll, also found through OOREXX_HOME).
"""
import sys
import oorexx

with oorexx.Interpreter() as rexx:
    version = rexx.eval(".rexxInfo~version 'r'.rexxInfo~revision")
    print(f"ooRexx/Python: Python {sys.version.split()[0]}, ooRexx {version}")
    got = rexx.routine("use arg a; return a~reverse")("Python")
    if got != "nohtyP":
        print(f"ooRexx/Python: FAILED (got {got!r})")
        sys.exit(1)
print("ooRexx/Python: OK")
