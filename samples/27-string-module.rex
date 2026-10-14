/* 27-string-module.rex: number-looking text that must stay text.
 *
 * A Rexx string that looks like a number goes to Python as a number: 2026
 * as the int 2026.  Where it is text (a file name, a code), .py~str(x)
 * keeps it a str; or a module imported as "string" sends every Rexx string
 * as a str, in all calls through it and through what it returns.
 * Positions and keys in o[k] keep the rule.
 *
 * Python:
 *   import posixpath      # os.path's Unix flavour: the same on every system
 *   print(posixpath.join("reports", str(2026), "07"))
 *   print(posixpath.join("reports", "2026", "07"))
 *   parts = posixpath.split("reports/2026/07"); print(parts[0])
 */
path = .py~import("posixpath")
say path~join("reports", .py~str(2026), "07")    -- "07" is text already
say .py~type(2026)~__name__                      -- int: the rule
text = .py~import("posixpath", "string")
say text~join("reports", 2026, "07")             -- all str
parts = text~split("reports/2026/07")
say parts[0]                                     -- 0 is still a position

::requires "python.cls"
/* Output:
reports/2026/07
int
reports/2026/07
reports/2026
*/
