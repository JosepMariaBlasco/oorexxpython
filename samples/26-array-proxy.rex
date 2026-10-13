/* 26-array-proxy.rex: a Python list as a Rexx Array, counting from 1.
 *
 * o[i] is Python's indexing, from 0.  o~makeArrayProxy gives a list (a
 * tuple, a 1-D numpy array, any sequence) as an object that behaves as a
 * Rexx Array: positions from 1, at, put, items, append, remove, DO OVER.
 * It reads and writes through to the Python object: nothing is copied.
 *
 * Python:
 *   days = ["Mon", "Tue", "Wed"]
 *   print(days[0], len(days))
 *   days[1] = "TUE"; days.append("Thu")
 *   for d in days: print(d)
 *   print(days)
 */
days = .py~eval('["Mon", "Tue", "Wed"]')
a = days~makeArrayProxy
say a[1] a~items                          -- 1 is the first, as in Rexx
a[2] = "TUE"; a~append("Thu")
do d over a
  say d
end
say days                                  -- the Python list changed

::requires "python.cls"
/* Output:
Mon 3
Mon
TUE
Wed
Thu
['Mon', 'TUE', 'Wed', 'Thu']
*/
