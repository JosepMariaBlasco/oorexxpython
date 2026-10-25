/* 28-address.rex: an ADDRESS environment written in Python, from Rexx.
 *
 * A Python callable is a command handler like any other of ooRexx's:
 * .py~commandHandler(handler) makes it one, and .CommandHandler~register
 * makes it the environment.  It gets the command (a str) and ooRexx's
 * .CommandContext, and what it returns is RC, whatever its sign (ERROR or
 * FAILURE only if it raises them).  .CommandHandler~deregister(name) removes
 * it: the name is unknown again, FAILURE RC 30, as for any unknown
 * environment.  (32-address.py does the same from Python.)
 *
 * Python:
 *   def calc(command, ctx):
 *       print(f"{command} = {eval(command)}", flush=True)
 *       return eval(command)
 */
.py~exec("def calc(command, ctx):"                                 || "0a"x || -
         "    print(f'{command} = {eval(command)}', flush=True)"   || "0a"x || -
         "    return eval(command)")
.CommandHandler~register("calc", .py~commandHandler(.py~main~getattr("calc")))

address calc
'6 * 7';   say "RC" rc
'2 - 5';   say "RC" rc                    -- negative: still just RC

.CommandHandler~deregister("calc")
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
