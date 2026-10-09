/* 03-names.rex: Python names from Rexx, which upper-cases its symbols.
 *
 * o~name finds the attribute whatever its case: the lowercase name first,
 * else the only name that matches.  getattr/setattr use the exact name.
 *
 * Python:
 *   import collections
 *   c = collections.Counter("mississippi")
 *   print(c.most_common(2))
 *   print(getattr(c, "most_common")(1))
 */
collections = .py~import("collections")
c = collections~Counter("mississippi")   -- Counter, not COUNTER
say c~most_common(2)
say c~getattr("most_common")~new(1)       -- exact name; ~new calls it

::requires "python.cls"
/* Output:
[('i', 4), ('s', 4)]
[('i', 4)]
*/
