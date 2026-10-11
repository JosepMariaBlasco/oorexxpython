"""32-address.py: an ADDRESS environment written in Python.

Rexx sends commands with ADDRESS name 'command'; the Python function gets the
command and a context (the caller's variables, I/O redirection) and returns RC.
"""
import oorexx

def calc(command, ctx):
    """ADDRESS CALC: evaluate a Python expression, put it in the variable RESULT."""
    try:
        ctx.set_var("RESULT", eval(command, {"__builtins__": {}, "max": max}))
        return 0
    except Exception as e:
        ctx.set_var("RESULT", str(e))
        return 1

rexx = oorexx.Interpreter(command_environments={"CALC": calc})
program = rexx.routine("""
address calc
'2 ** 10';      say rc result
'max(3, 9, 4)'; say rc result
'1 / 0';        say rc result
""")
program()
rexx.close()

# Output:
# REXXout>0 1024
# REXXout>0 9
# REXXout>1 division by zero
