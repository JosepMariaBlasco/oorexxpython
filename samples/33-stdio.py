"""33-stdio.py: Rexx's standard streams go through Python's sys.stdout,
sys.stderr and sys.stdin, every Rexx line marked with a prefix.

So Python can capture what a Rexx program says, or feed it input, and in a
notebook or a log file Rexx's lines stand out (REXXout>, REXXerr>...).
"""
import contextlib, io, sys
import oorexx

rexx = oorexx.Interpreter()
print("from Python")
rexx.routine("say 'from Rexx'")()

chatty = rexx.routine("do i = 1 to 2; say 'line' i; end; .error~lineout('a warning')")
out, err = io.StringIO(), io.StringIO()
with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
    chatty()
print("stdout:", out.getvalue().splitlines())
print("stderr:", err.getvalue().splitlines())
rexx.close()

plain = oorexx.Interpreter(prefixes=False)       # no prefixes
sys.stdin = io.StringIO("Josep Maria\n")
print(plain.routine("say 'Your name?'; parse pull name; return 'Hi,' name")())
plain.close()

# Output:
# from Python
# REXXout>from Rexx
# stdout: ['REXXout>line 1', 'REXXout>line 2']
# stderr: ['REXXerr>a warning']
# Your name?
# Hi, Josep Maria
