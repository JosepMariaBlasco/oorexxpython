/* 04-keywords.rex: keyword arguments.
 *
 * .py~kw(name, value, ...) as the LAST argument of a call; py.kw(...) is the
 * same as a routine.  A Directory works too: .py~kw(aDirectory).
 *
 * Python:
 *   words = ["pear", "Fig", "banana"]
 *   print(sorted(words, key=str.lower, reverse=True))
 *   print(int("ff", base=16))
 *   opts = {"sep": " | ", "maxsplit": 1}
 *   print("a | b | c".split(**opts))
 */
words = .array~of("pear", "Fig", "banana")
say .py~sorted(words, .py~kw("key", .py~eval("str.lower"), "reverse", .true))
say .py~int("ff", py.kw("base", 16))
opts = .directory~new
opts["sep"] = " | "                   -- [] keeps the lowercase key
opts["maxsplit"] = 1
say .py~str("a | b | c")~split(.py~kw(opts))

::requires "python.cls"
/* Output:
['pear', 'Fig', 'banana']
255
['a', 'b | c']
*/
