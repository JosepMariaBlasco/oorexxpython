/* 29-throw.rex: thrown conditions go through Python, both ways.
 *
 * Rexx's THROW is Python's raise: the condition goes from caller to caller
 * until a CATCH clause catches it, also when Python is in between (here
 * Python's sum() and map() call the Rexx routine that throws).  And Python
 * throws a Rexx condition with oorexx.throw(condition, rc, description,
 * additional), which is Rexx's THROW with the same options.
 *
 * Python:
 *   class NoPrice(Exception): pass
 *   def price(item):
 *       if item not in prices: raise NoPrice("no price for " + item)
 *       return prices[item]
 *   try:
 *       print(sum(map(price, ["pear", "fig"])))
 *       print(sum(map(price, ["pear", "kiwi"])))
 *   except NoPrice as e:
 *       print("caught:", e)
 */
do
  say total(.array~of("pear", "fig"))
  say total(.array~of("pear", "kiwi"))
  say "not reached"
catch c = user noPrice                    -- Python's except NoPrice
  say "caught" c~condition":" c~description
end

-- Python throws a Rexx condition
.py~exec("import oorexx"                                                 || "0a"x || -
         "def positive(n):"                                              || "0a"x || -
         "    if n < 0:"                                                 || "0a"x || -
         "        oorexx.throw('user negative', description='%d is negative' % n, additional=[n])" || "0a"x || -
         "    return n")
do
  say .py~main~positive(5)
  say .py~main~positive(-1)
catch c = user negative
  say "caught" c~condition":" c~description "(additional:" c~additional[1]")"
end

::routine total                           -- no CATCH here: the condition goes on
  use arg basket
  return .py~sum(.py~map(.context~package~findRoutine("PRICE"), basket))

::routine price                           -- called by Python
  use arg item
  if item == "pear" then return 3
  if item == "fig" then return 2
  throw user noPrice description ("no price for" item)

::requires "python.cls"
/* Output:
5
caught USER NOPRICE: no price for kiwi
5
caught USER NEGATIVE: -1 is negative (additional: -1)
*/
