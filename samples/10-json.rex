/* 10-json.rex: JSON with Python's json module.
 *
 * Python:
 *   import json
 *   text = json.dumps({"lang": "ooRexx", "since": 2004, "oo": True}, sort_keys=True)
 *   print(text)
 *   data = json.loads('{"langs": ["Rexx", "Python"], "year": 2026}')
 *   print(data["langs"][1], data["year"])
 */
json = .py~import("json")
d = .py~dict(.py~kw("lang", "ooRexx", "since", 2004, "oo", .Boolean~true))
text = json~dumps(d, .py~kw("sort_keys", .true))
say text
data = json~loads('{"langs": ["Rexx", "Python"], "year": 2026}')
say data["langs"][1] data["year"]
do lang over data["langs"]
  say "-" lang
end

::requires "python.cls"
/* Output:
{"lang": "ooRexx", "oo": true, "since": 2004}
Python 2026
- Rexx
- Python
*/
