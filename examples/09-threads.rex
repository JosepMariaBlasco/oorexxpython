/* 09-threads.rex: Rexx threads and Python threads, together.
 *
 * Each Rexx thread (START) calls Python; the bridge takes Python's GIL for
 * the call, and releases it while Python runs Rexx code.
 *
 * Python:
 *   from concurrent.futures import ThreadPoolExecutor
 *   with ThreadPoolExecutor(4) as ex:
 *       print(sum(ex.map(lambda n: sum(range(n)), [10, 100, 1000, 10000])))
 */
workers = .array~new
do n over .array~of(10, 100, 1000, 10000)
  workers~append(.Summer~new~start("sumTo", n))   -- one Rexx thread each
end
total = 0
do w over workers
  total += w~result
end
say total

-- and Python's thread pool calling a Rexx routine
cf = .py~import("concurrent.futures")
ex = cf~ThreadPoolExecutor(4)
say .py~sum(ex~map(.context~package~findRoutine("sumTo"), .array~of(10, 100, 1000, 10000)))
ex~shutdown

::class Summer
::method sumTo
  return .py~sum(.py~range(arg(1)))

::routine sumTo
  return .py~sum(.py~range(arg(1)))

::requires "python.cls"
/* Output:
50499495
50499495
*/
