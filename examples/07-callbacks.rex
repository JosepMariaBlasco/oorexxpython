/* 07-callbacks.rex: Rexx code called from Python.
 *
 * A Rexx object passed to Python is an oorexx.RexxObject there: calling it
 * sends CALL (so a Routine is a Python function), o.name(...) sends a message.
 *
 * Python:
 *   def by_length(s): return len(s)
 *   print(sorted(["pear", "fig", "banana"], key=by_length))
 *   print(sum(map(lambda x: x * x, [1, 2, 3])))
 */
words = .array~of("pear", "fig", "banana")
byLength = .context~package~findRoutine("byLength")
say .py~sorted(words, .py~kw("key", byLength))
say .py~sum(.py~map(.context~package~findRoutine("square"), .array~of(1, 2, 3)))

-- Python sends messages to a Rexx object
.py~exec("def shout(o): return o.upper() + '!'")
say .py~main~shout("hello")                   -- a Rexx string is a Python str
.py~exec("def total(acct): return acct.balance() + 10")
say .py~main~total(.Account~new)              -- a Rexx object stays Rexx

::routine byLength
  return length(arg(1))
::routine square
  return arg(1) ** 2

::class Account
::method balance
  return 90

::requires "python.cls"
/* Output:
['fig', 'pear', 'banana']
14
HELLO!
100
*/
