/* 15-random.rex: random numbers with Python's random module (seeded, so the
 * output is the same on every run).
 *
 * Python:
 *   import random
 *   random.seed(2026)
 *   print([random.randint(1, 6) for _ in range(5)])
 *   print(random.choice(["Rexx", "Python", "Java"]))
 */
random = .py~import("random")
random~seed(2026)
dice = ""
do 5
  dice = dice random~randint(1, 6)
end
say dice~strip
say random~choice(("Rexx", "Python", "Java"))

::requires "python.cls"
/* Output:
1 3 5 5 6
Rexx
*/
