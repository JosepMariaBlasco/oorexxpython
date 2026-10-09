"""31-rexx-objects.py: Rexx objects used from Python.

A Rexx object is an oorexx.RexxObject in Python: o.name(...) sends a message,
o[k] is Rexx's [], len() and iteration work, str() is its string value.
A Rexx Array comes back as a Python list (a copy).
"""
import oorexx

with oorexx.Interpreter() as rexx:
    d = rexx.eval(".directory~new")
    d["NAME"] = "Ann"
    d["AGE"] = 31
    print(len(d), d["NAME"], d.hasIndex("AGE"), sorted(d.allIndexes()))
    print(rexx.eval(".array~of(3, 1, 2)~sort"))
    stack = rexx.eval(".queue~new")
    for word in "to be or not".split():
        stack.push(word)
    print([w for w in stack])
    print(str(rexx.eval(".dateTime~new(2026, 10, 7)~standardDate")))

# Output:
# 2 Ann 1 ['AGE', 'NAME']
# [1, 2, 3]
# ['not', 'or', 'be', 'to']
# 20261007
