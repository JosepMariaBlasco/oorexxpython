/* 01-hello.rex: import a Python module and call it.
 *
 * Python:
 *   import math
 *   print(math.sqrt(2))
 *   print(math.pi)
 *   print(math.factorial(30))
 */
math = .py~import("math")
say math~sqrt(2)
say math~pi
say math~factorial(30)        -- Python ints have no size limit

::requires "python.cls"
/* Output:
1.4142135623730951
3.141592653589793
265252859812191058636308480000000
*/
