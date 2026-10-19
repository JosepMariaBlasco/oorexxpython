/* 28-address.rex: an ADDRESS environment written in Python, from Rexx.
 *
 * .py~addCommandEnvironment(name, handler) makes a Python callable an ADDRESS
 * environment: it gets the command and a context, and what it returns is RC,
 * whatever its sign (ERROR or FAILURE only if it raises them).  Without WITH,
 * what it writes goes to the console.  .py~removeCommandEnvironment(name)
 * makes the name unknown again: FAILURE, RC 30, as for any unknown
 * environment (removing a name that is not registered is an error).
 * (32-address.py does the same from Python.)
 *
 * Python:
 *   def calc(command, ctx):
 *       ctx.output(f"{command} = {eval(command)}")
 *       return eval(command)
 */
.py~exec("def calc(command, ctx):"                     || "0a"x || -
         "    ctx.output(f'{command} = {eval(command)}')" || "0a"x || -
         "    return eval(command)")
.py~addCommandEnvironment("calc", .py~main~getattr("calc"))

address calc
'6 * 7';   say "RC" rc
'2 - 5';   say "RC" rc                    -- negative: still just RC

.py~removeCommandEnvironment("calc")
trace off                                 -- no trace line for the failure
signal on failure
'1 + 1'
exit
failure:
  say condition("C") "RC" rc

::requires "python.cls"
/* Output:
6 * 7 = 42
RC 42
2 - 5 = -3
RC -3
FAILURE RC 30
*/
