/* 02-values.rex: which values cross as Rexx strings, which stay Python objects.
 *
 * Python str, int, float and None come back as Rexx strings (.nil for None),
 * bool as .LogicalWrapper~true or ~false (1 and 0 for Rexx); everything else
 * is a .PyObject, a reference to the Python object.
 * Rexx numbers go to Python as int or float; other strings as str.
 *
 * Python:
 *   print(type(42).__name__, type(3.5).__name__, type("007").__name__)
 *   print(len("Rexx"), divmod(7, 2)[0], True)
 *   d = {}; print(type(d).__name__)
 */
say .py~type(42)~__name__ .py~type(3.5)~__name__ .py~type("007")~__name__
say .py~len("Rexx") .py~divmod(7, 2)[0] .py~bool(1)
d = .py~dict~new                  -- a Python dict: stays in Python
say d~class~id .py~type(d)~__name__
s = .py~str(42)                   -- force a Python str, kept as an object
say .py~type(s)~__name__ s~zfill(5)
say .py~eval("None") == .nil

::requires "python.cls"
/* Output:
int float str
4 3 1
PYOBJECT dict
str 00042
1
*/
