# ooRexx bugs found by the bridge

The bridge's tests found these bugs in ooRexx's native API and memory
management. Each is reported on SourceForge
(`https://sourceforge.net/p/oorexx/bugs/NNNN/`), with a reproducer and a
fix. The code marks the workarounds with the ticket number, so
`grep -n '#21' src/*` finds them.

| Ticket | What | Fixed in trunk | What the bridge does |
|---|---|---|---|
| [#2100](https://sourceforge.net/p/oorexx/bugs/2100/) | `ReleaseGlobalReference` adds a reference instead of releasing one, so nothing released that way is ever freed | r13257 | Rexx objects held by Python live in a per-instance Directory, not as global references (`InstanceData::keep` in `bridge.cpp`) |
| [#2101](https://sourceforge.net/p/oorexx/bugs/2101/) | `runUninits()` crashes or hangs when an uninit method gives up the kernel lock while other threads create objects with uninit methods | r13256 | `.PyObject`'s uninit is two Rexx clauses that only queue the pointer; the bridge drops the Python reference at its next call (`drainGraveyard`) |
| [#2102](https://sourceforge.net/p/oorexx/bugs/2102/) | `pendingUninits` only grows, so after a while every call scans the whole uninit table | r13259 | nothing can avoid it (every proxy has an uninit): use an ooRexx with the fix for serious work |
| [#2103](https://sourceforge.net/p/oorexx/bugs/2103/) | an ending thread uses its Activity after the garbage collector may have swept it: a rare crash at exit under heavy threading | r13265 | nothing in the bridge; seen only in the heavy run of `Threads.testGroup` |
| [#2122](https://sourceforge.net/p/oorexx/bugs/2122/) | `IsInstanceOf` (also `IsRoutine`, `IsMethod`) is a non-blocking API call, but a number with no text yet (the result of `j + 0`) answers it by making its string, an allocation without the kernel lock: heap corruption when other threads run Rexx (crashes, hangs in the collector, wrong values) | r13286 | nothing: the minimum, r13324, has the fix (until r13286 `instanceOf()` in `bridge.cpp` never asked `IsInstanceOf` of a string or a number); seen as a rare crash of `t50_subclass.py`'s eight threads |
| [#2107](https://sourceforge.net/p/oorexx/bugs/2107/) | `Terminate` waits forever if the thread that created the instance called `DetachThread` | open | the creating thread stays attached until `Interpreter.close()` |

The workarounds above stay right on older versions, and none of them
depends on a fix being present. #2122 has none any more: the bridge needs
its fix. The design notes behind them are in
[`../docs/design.md`](../docs/design.md), *Lifetime*.

The bridge needs ooRexx 5.3.0 r13324 or later (`logicalwrapper.cls`, since
r13278, #2122's fix, since r13286, #2123's, since r13320, the ooRexx bridges' revision), so the fixes above are all present. Before that, ooRexx 5.2.0 was the minimum for
using an interpreter from one thread at a time: from several at once it
crashed inside ooRexx (in its tables of activities, on `AttachThread` and
when a thread waits for the kernel lock), and `Threads.testGroup`,
`tests/python/t15_concurrent_calls.py` and
`tests/python/t20_returned_objects.py` failed with it; 5.3.0 from r13265
passed them.

Tickets filed for other reasons that also touch the bridge:

- [#2105](https://sourceforge.net/p/oorexx/bugs/2105/) (fixed in r13266):
  `ADDRESS ... WITH INPUT USING (aFile)` sent no input, so an ADDRESS
  environment written in Python got no lines from `ctx.lines()` in that
  form. `WITH INPUT STREAM` and `STEM` were not affected.
- [#2106](https://sourceforge.net/p/oorexx/bugs/2106/) (fixed in r13267):
  entering the interpreter was slow on Linux. No measurable effect on the
  bridge's calls.
- [#2113](https://sourceforge.net/p/oorexx/bugs/2113/) (fixed in r13271):
  the `Is...Redirected` calls an ADDRESS handler makes could silently clear a
  condition pending on the handler's activation. The bridge's ADDRESS
  environments written in Python make those calls (`ctx.is_redirected`,
  `ctx.line_in`, `ctx.output`, `ctx.error`).
- [#2114](https://sourceforge.net/p/oorexx/bugs/2114/) (fixed in r13270): a
  `SELECT CASE` whose expression allocates can corrupt the heap if a garbage
  collection runs meanwhile. It touches any Rexx program; the bridge's own
  Rexx code has no `SELECT CASE`.
- [#2111](https://sourceforge.net/p/oorexx/bugs/2111/) (fixed in r13274):
  `AddCommandEnvironment` changed the instance's table of environments
  without the kernel lock. Registering an ADDRESS environment with
  `Interpreter.add_command_environment` while Rexx runs on another thread of
  the same instance could corrupt the heap. Before r13274, register
  environments when the interpreter is created (`command_environments=`) or
  before other threads run Rexx.
- [#2112](https://sourceforge.net/p/oorexx/bugs/2112/) (fixed in r13275):
  resolving an ADDRESS environment missing from the instance's table, while
  other threads ran Rexx, could register a freed handler. Environments
  registered through the bridge are in the table; addressing one before it
  is registered was the risky case.
- `tests/python/t35_address_threads.py` does both at once (environments
  registered while eight threads address them). On r13271 it fails 24 times
  in 30 (crashes, a hang, lost registrations); on r13275, 0 in 200. It is
  skipped before r13275.
- [#2116](https://sourceforge.net/p/oorexx/bugs/2116/) (fixed in r13324;
  filed by Tom Dyer): an argument error raised by a native method (wrong
  number of arguments, an argument its signature cannot convert) was
  invisible to the native code that sent the message from its own *method*
  context: `CheckCondition()` was false, and a later garbage collection could
  crash. Its first part, `ReleaseGlobalReference`, is #2100 above. The
  bridge was not exposed: every message it sends with a user's arguments
  goes through a thread context of its own (`AttachThread`), where the
  condition is seen and nothing is left behind (checked from Python and
  from Rexx through Python, with a collection after each failed call).
- [#2123](https://sourceforge.net/p/oorexx/bugs/2123/) (fixed in r13320):
  a condition other than SYNTAX (USER, NOTREADY, ERROR, FAILURE...) raised
  by Rexx code that native code calls never reached the native caller, so
  it could not go through Python back to Rexx as itself. The bridge needed
  no change (it already kept any condition and raised it again); the tests
  now check every one of them both ways.
- [#2126](https://sourceforge.net/p/oorexx/bugs/2126/) (fixed in r13317)
  and [RFE #883](https://sourceforge.net/p/oorexx/feature-requests/883/)
  (open): native code cannot raise a SYNTAX condition again with its
  `DESCRIPTION`. `RaiseException(code, additional)` takes none, and
  `RaiseCondition("SYNTAX", ...)` crashed the process (an error 93.900 since
  #2126). The bridge never calls `RaiseCondition` with `SYNTAX`; it raises
  the error with `RaiseException` and then puts the description and the
  result into the pending condition object that `GetConditionInfo()`
  answers (`raiseAgain` in `bridge.cpp`, marked as a workaround), so a SYNTAX
  error that goes through Python back to Rexx keeps them
  (`test_rexx_error_description` in `Callbacks.testGroup`). RFE #883 asks
  for an API that raises a condition object as it is.

Not a bug, but worth knowing when writing native code: `IsString` is false
for strings the interpreter keeps in another form (internal integers, number
strings, subclasses of `.String`). It promises only that the object "may be
safely cast to a RexxStringObject". The bridge uses `IsOfType(o, "String")`
and `ObjectToString` for "is a string value".
