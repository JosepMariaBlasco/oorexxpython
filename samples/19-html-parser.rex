/* 19-html-parser.rex: Python's html.parser, with the handler written in Rexx.
 *
 * HTMLParser is used by subclassing: the Rexx class Links extends it
 * (.py~extend), and its handle_starttag is the Rexx method
 * (24-subclass.rex has more).
 *
 * Python:
 *   from html.parser import HTMLParser
 *   class Links(HTMLParser):
 *       def handle_starttag(self, tag, attrs):
 *           if tag == "a": print(dict(attrs)["href"])
 *   Links().feed('<p><a href="https://rexxla.org">RexxLA</a> <a href="/x">x</a></p>')
 */
.Links~new~feed('<p><a href="https://rexxla.org">RexxLA</a> <a href="/x">x</a></p>')

::class Links subclass PyObject
::method activate class
  .py~extend(self, "html.parser.HTMLParser")
::method handle_starttag
  use arg tag, attrs
  if tag == "a" then say .py~dict(attrs)["href"]

::requires "python.cls"
/* Output:
https://rexxla.org
/x
*/
