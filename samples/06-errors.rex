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
  -- the Python traceback, as text, is in additional[2]
  say "Python says:" co~additional[2]~strip("T", "0a"x)

::requires "python.cls"
/* Output:
caught 98.900: Python error: ValueError: invalid literal for int() with base 10: 'abc'.
Python says: ValueError: invalid literal for int() with base 10: 'abc'
*/
