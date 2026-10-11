/* 19-html-parser.rex: Python's html.parser, with the handler written in Rexx.
 *
 * HTMLParser is used by subclassing; a three-line Python subclass forwards
 * each event to a Rexx object.
 *
 * Python:
 *   from html.parser import HTMLParser
 *   class Links(HTMLParser):
 *       def handle_starttag(self, tag, attrs):
 *           if tag == "a": print(dict(attrs)["href"])
 *   Links().feed('<p><a href="https://rexxla.org">RexxLA</a> <a href="/x">x</a></p>')
 */
nl = "0a"x
.py~exec("from html.parser import HTMLParser"                   || nl ||,
         "class ToRexx(HTMLParser):"                            || nl ||,
         "    def __init__(self, handler):"                     || nl ||,
         "        super().__init__(); self.handler = handler"   || nl ||,
         "    def handle_starttag(self, tag, attrs):"           || nl ||,
         "        self.handler.starttag(tag, dict(attrs))")
parser = .py~main~ToRexx(.Links~new)
parser~feed('<p><a href="https://rexxla.org">RexxLA</a> <a href="/x">x</a></p>')

::class Links
::method starttag
  use arg tag, attrs
  if tag == "a" then say attrs["href"]

::requires "python.cls"
/* Output:
https://rexxla.org
/x
*/
