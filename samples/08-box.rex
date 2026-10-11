/* 08-box.rex: py.box: choose the Python type of a Rexx value.
 *
 * Rexx strings that look like numbers go to Python as int or float.  When
 * another type is wanted, box it: py.box(type, value), as BSF4ooRexx's box, with
 * type str, int, float, bool, bytes, complex, decimal, fraction, or a
 * Python class.
 *
 * Python:
 *   from fractions import Fraction
 *   from decimal import Decimal
 *   print(repr("42"), repr(42.0), repr(b"ab"))
 *   print(Fraction(1, 3) + Fraction(1, 6))
 *   print(0.1 + 0.2, Decimal("0.1") + Decimal("0.2"))
 */
say .py~repr(py.box("str", 42)) .py~repr(py.box("float", 42)) .py~repr(py.box("bytes", "ab"))
say py.box("fraction", "1/3") + py.box("fraction", "1/6")
say .py~eval("0.1 + 0.2") (py.box("decimal", "0.1") + py.box("decimal", "0.2"))

::requires "python.cls"
/* Output:
'42' 42.0 b'ab'
1/2
0.30000000000000004 0.3
*/
