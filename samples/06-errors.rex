/* 06-errors.rex: a Python exception is a Rexx SYNTAX condition (98.900).
 *
 * Python:
 *   try:
 *       int("abc")
 *   except ValueError as e:
 *       print("caught:", e)
 */
signal on syntax
say .py~int("abc")
say "not reached"
exit

syntax:
  co = condition("O")
  say "caught" co~code":" co~message
  -- the exception itself is in additional[2], its traceback (text) in [3]
  say "the exception:" .py~type(co~additional[2])~getattr("__name__")
  say "Python says:" co~additional[3]~strip("T", "0a"x)

::requires "python.cls"
/* Output:
caught 98.900: Python error: ValueError: invalid literal for int() with base 10: 'abc'.
the exception: ValueError
Python says: ValueError: invalid literal for int() with base 10: 'abc'
*/
