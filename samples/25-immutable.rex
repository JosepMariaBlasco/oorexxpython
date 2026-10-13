/* 25-immutable.rex: Rexx methods on a str, an int or a tuple.
 *
 * An immutable Python object gets its value when it is made, in __new__:
 * INIT's self~init:super(...) passes it there, so Tag~new("b") can make a
 * str whose value is "<b>".  The object is a str in Python and a Tag in
 * Rexx, with the methods of both.
 *
 * Python:
 *   class Tag(str):
 *       def __new__(cls, name): return super().__new__(cls, "<" + name + ">")
 *       def close(self): return "</" + self[1:]
 *   b = Tag("b")
 *   print(b + "bold" + b.close(), isinstance(b, str))
 *   class Celsius(int):
 *       def fahrenheit(self): return self * 9 // 5 + 32
 *   print(Celsius(100).fahrenheit())
 */
b = .Tag~new("b")
say b || "bold" || b~close .py~isinstance(b, .py~getattr("str"))

say .Celsius~new(100)~fahrenheit

::class Tag subclass PyObject
::method activate class
  .py~extend(self, "str")
::method init
  use arg name
  self~init:super("<" || name || ">")
::method close
  return "</" || self~string~substr(2)

::class Celsius subclass PyObject
::method activate class
  .py~extend(self, "int")
::method fahrenheit
  return self * 9 / 5 + 32

::requires "python.cls"
/* Output:
<b>bold</b> 1
212
*/
