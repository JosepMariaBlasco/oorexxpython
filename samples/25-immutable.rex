/* 25-immutable.rex: Rexx methods on a str, an int or a tuple.
 *
 * An immutable Python object gets its value in __new__, not __init__:
 * .py~extend passes the constructor's arguments to the base's __new__, so
 * Tag~new("b") is a str whose value is "b".  A Rexx method __new__ can
 * change the value first: SELF is then the class, and SUPER~__new__ is the
 * base's.  Such objects stay Python objects in Rexx, with their methods.
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
Tag = .py~extend("str", .Tag)
b = Tag~new("b")
say b || "bold" || b~close .py~isinstance(b, .py~getattr("str"))

Celsius = .py~extend("int", .Celsius)
say Celsius~new(100)~fahrenheit

::class Tag
::method __new__
  use arg name, slot
  return slot~super~__new__(slot~self, "<" || name || ">")
::method close
  use arg slot
  return "</" || slot~self~string~substr(2)

::class Celsius
::method fahrenheit
  use arg slot
  return slot~self * 9 / 5 + 32

::requires "python.cls"
/* Output:
<b>bold</b> 1
212
*/
