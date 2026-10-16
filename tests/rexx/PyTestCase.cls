/*----------------------------------------------------------------------------*/
/* PyTestCase.cls: the base class of the bridge's ooRexxUnit test groups.     */
/*                                                                            */
/* Licensed under the Apache License, Version 2.0.                            */
/*----------------------------------------------------------------------------*/

::requires 'ooTest.frm'      -- ooRexxUnit (scripts/setup-oorexxunit.sh)
::requires 'python.cls'      -- the bridge

::class PyTestCase subclass ooTestCase public

/** syntaxOf(receiver, messageName [, args...])
 * Sends the message and returns the SYNTAX condition object it raises; the test
 * fails if none is raised.  (The bridge's errors carry more than a code: the
 * message, the Python exception in additional[2] and its traceback in
 * additional[3].)
 */
::method syntaxOf
  use strict arg receiver, name, ...
  args = arg(3, "A")
  signal on syntax
  .message~new(receiver, name, "A", args)~send
  signal off syntax
  self~fail("no SYNTAX condition raised by message" name)
syntax:
  return condition("O")

/** py(lines...)
 * Joins its arguments with line feeds: Python source for .py~exec.
 */
::method py
  src = ""
  do i = 1 to arg()
    src ||= arg(i) || "0a"x
  end
  return src
