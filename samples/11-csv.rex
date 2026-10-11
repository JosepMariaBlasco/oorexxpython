/* 11-csv.rex: read CSV text with Python's csv module.
 *
 * Python:
 *   import csv, io
 *   text = "name,score\nAnn,90\nBob,85\n"
 *   for row in csv.DictReader(io.StringIO(text)):
 *       print(row["name"], int(row["score"]) + 5)
 */
csv = .py~import("csv")
io  = .py~import("io")
text = "name,score" || "0a"x || "Ann,90" || "0a"x || "Bob,85" || "0a"x
do row over csv~DictReader(io~StringIO(text))
  say row["name"] row["score"] + 5          -- "90" is a Rexx string: Rexx adds
end

-- and write it
out = io~StringIO~new                       -- a class: ~new makes an instance
w = csv~writer(out)
w~writerow(("total", 175))
say out~getvalue~strip("T", "0a"x)~strip("T", "0d"x)

::requires "python.cls"
/* Output:
Ann 95
Bob 90
total,175
*/
