/* 05-items.rex: items and iteration: o[k], o[k] = v, DO OVER.
 *
 * Python:
 *   ages = {"Ann": 31, "Bob": 27}
 *   ages["Cy"] = 45
 *   for name in ages: print(name, ages[name])
 *   grid = [[1, 2], [3, 4]]; print(grid[1][0])
 */
ages = .py~dict~new
ages["Ann"] = 31
ages["Bob"] = 27
ages["Cy"]  = 45
do name over ages
  say name ages[name]
end
grid = .py~list(.array~of(.array~of(1, 2), .array~of(3, 4)))   -- Rexx Arrays go as lists
say grid[1][0]
say .py~len(ages) "people, average age" .py~sum(ages~values) / .py~len(ages)

::requires "python.cls"
/* Output:
Ann 31
Bob 27
Cy 45
3
3 people, average age 34.3333333
*/
