/* 22-slices.rex: slices: o[.py~slice(start, stop[, step])] is Python's o[start:stop:step].
 *
 * Python:
 *   text = "ooRexx and Python"
 *   print(text[0:6], text[-6:], text[::-1])
 *   nums = list(range(10)); print(nums[2:8:2]); nums[0:3] = ["a"]; print(nums)
 *   m = numpy.arange(12).reshape(3, 4); print(m[0:2, 1])
 */
text = .py~str("ooRexx and Python")      -- a Python str (a Rexx string would not slice)
say text[.py~slice(0, 6)] text[.py~slice(-6, .nil)] text[.py~slice(.nil, .nil, -1)]
nums = .py~list(.py~range(10))
say nums[.py~slice(2, 8, 2)]
nums[.py~slice(0, 3)] = .array~of("a")
say nums
np = .py~import("numpy")
m = np~arange(12)~reshape(3, 4)
say m[.py~slice(0, 2), 1]                -- m[0:2, 1]: rows 0 and 1, column 1

::requires "python.cls"
/* Output:
ooRexx Python nohtyP dna xxeRoo
[2, 4, 6]
['a', 3, 4, 5, 6, 7, 8, 9]
[1 5]
*/
