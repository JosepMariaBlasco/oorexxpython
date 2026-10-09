/* 17-urllib.rex: URLs with Python's urllib.parse (urllib.request would fetch
 * them: urllib~request~urlopen(url)~read).
 *
 * Python:
 *   from urllib.parse import urlparse, urlencode, quote
 *   u = urlparse("https://www.rexxla.org/events/2026.html?lang=en#talks")
 *   print(u.scheme, u.netloc, u.path, u.query, u.fragment)
 *   print(urlencode({"q": "ooRexx Python", "page": 2}))
 */
parse = .py~import("urllib.parse")
u = parse~urlparse("https://www.rexxla.org/events/2026.html?lang=en#talks")
say u~scheme u~netloc u~path u~query u~fragment
say parse~urlencode(.py~dict(.py~kw("q", "ooRexx Python", "page", 2)))
say parse~quote("a b/c")

::requires "python.cls"
/* Output:
https www.rexxla.org /events/2026.html lang=en talks
q=ooRexx+Python&page=2
a%20b/c
*/
