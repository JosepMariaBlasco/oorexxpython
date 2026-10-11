/* 18-logging.rex: Python's logging module, here into a string buffer.
 *
 * Python:
 *   import logging, io
 *   buf = io.StringIO()
 *   log = logging.getLogger("demo")
 *   h = logging.StreamHandler(buf)
 *   h.setFormatter(logging.Formatter("%(levelname)s %(name)s: %(message)s"))
 *   log.addHandler(h); log.setLevel(logging.INFO)
 *   log.info("started"); log.debug("not shown"); log.warning("careful")
 *   print(buf.getvalue(), end="")
 */
logging = .py~import("logging")
buf = .py~import("io")~StringIO~new
log = logging~getLogger("demo")
h = logging~StreamHandler(buf)
h~setFormatter(logging~Formatter("%(levelname)s %(name)s: %(message)s"))
log~addHandler(h)
-- logging~INFO would find logging.info (Rexx symbols are upper case, and the
-- lowercase name wins): the constant needs its exact name
log~setLevel(logging~getattr("INFO"))
log~info("started")
log~debug("not shown")
log~warning("careful")
do line over buf~getvalue~makeArray("0a"x)
  say line
end

::requires "python.cls"
/* Output:
INFO demo: started
WARNING demo: careful
*/
