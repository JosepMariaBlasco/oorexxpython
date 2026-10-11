"""33-stdio.py: Rexx's SAY and PULL go through Python's sys.stdout/sys.stdin.

So Python can capture what a Rexx program says, or feed it input.
"""
import contextlib, io, sys
import oorexx

rexx = oorexx.Interpreter()
chatty = rexx.routine("do i = 1 to 3; say 'line' i; end")
buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    chatty()
print("captured:", buf.getvalue().splitlines())

sys.stdin = io.StringIO("Josep Maria\n")
print(rexx.routine("say 'Your name?'; parse pull name; return 'Hi,' name")())
rexx.close()

# Output:
# captured: ['line 1', 'line 2', 'line 3']
# Your name?
# Hi, Josep Maria
