/* 13-sqlite3.rex: an SQL database with Python's sqlite3 module.
 *
 * Python:
 *   import sqlite3
 *   db = sqlite3.connect(":memory:")
 *   db.execute("create table langs (name text, year int)")
 *   db.executemany("insert into langs values (?, ?)", [("Rexx", 1979), ("Python", 1991)])
 *   for name, year in db.execute("select name, year from langs order by year"):
 *       print(name, year)
 */
sqlite3 = .py~import("sqlite3")
db = sqlite3~connect(":memory:")
db~execute("create table langs (name text, year int)")
rows = ("Rexx", 1979), ("Python", 1991), ("ooRexx", 2004)
db~executemany("insert into langs values (?, ?)", rows)
do row over db~execute("select name, year from langs order by year")
  say row[0]~left(8) row[1]
end
say db~execute("select count(*) from langs")~fetchone[0] "rows"
db~close

::requires "python.cls"
/* Output:
Rexx     1979
Python   1991
ooRexx   2004
3 rows
*/
