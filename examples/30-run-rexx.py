"""30-run-rexx.py: Python runs Rexx code.

An oorexx.Interpreter is an ooRexx interpreter instance: eval an expression,
make a routine from source, run a program file.  Results follow the same
rule as the other way round: Rexx numbers come back as int/float.
"""
import os, tempfile
import oorexx

with oorexx.Interpreter() as rexx:
    print(rexx.eval("2 ** 100"))              # Rexx's 1.26765060E+30 (9 digits): a float
    print(rexx.routine("numeric digits 40; return 2 ** 100")())   # exact: an int
    print(rexx.eval("'Hello'~reverse~upper"))
    area = rexx.routine("use arg w, h; return w * h")
    print(area(6, 7), type(area(6, 7)).__name__)
    folder = tempfile.mkdtemp()
    with open(os.path.join(folder, "greet.rex"), "w") as f:
        f.write("parse arg who\nsay 'Hello,' who'!'\nreturn length(who)\n")
    print(rexx.run(os.path.join(folder, "greet.rex"), "ooRexx"))

# Output:
# 1.2676506e+30
# 1267650600228229401496703205376
# OLLEH
# 42 int
# Hello, ooRexx!
# 6
