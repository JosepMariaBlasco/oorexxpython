/* 14-datetime.rex: dates with Python's datetime module.
 *
 * Python:
 *   from datetime import date, timedelta
 *   d = date(2026, 10, 7)
 *   print(d.isoformat(), d.strftime("%A"), d + timedelta(days=30))
 *   print((date(2026, 12, 25) - d).days)
 */
dt = .py~import("datetime")
d = dt~date(2026, 10, 7)
say d~isoformat d~strftime("%A") d + dt~timedelta(.py~kw("days", 30))
say (dt~date(2026, 12, 25) - d)~days "days to Christmas"

::requires "python.cls"
/* Output:
2026-10-07 Wednesday 2026-11-06
79 days to Christmas
*/
