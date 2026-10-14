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
| [#2107](https://sourceforge.net/p/oorexx/bugs/2107/) | `Terminate` waits forever if the thread that created the instance called `DetachThread` | open | the creating thread stays attached until `Interpreter.close()` |

The workarounds above stay right on older versions, and none of them
depends on a fix being present. The design notes behind them are in
[`../docs/design.md`](../docs/design.md), *Lifetime*.

ooRexx 5.2.0, the current release, is the minimum only for using an
interpreter from one thread at a time: from several at once it crashes
inside ooRexx (in its tables of activities, on `AttachThread` and when a
thread waits for the kernel lock), and `Threads.testGroup`,
`tests/python/t15_concurrent_calls.py` and
`tests/python/t20_returned_objects.py` fail with it. 5.3.0 from r13265
passes them; which of the fixes since 5.2.0 makes the difference has not
been pinned down (`docs/design.md`, *ooRexx versions*).

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
- [#2111](https://sourceforge.net/p/oorexx/bugs/2111/) (open):
  `AddCommandEnvironment` changes the instance's table of environments
  without the kernel lock. Registering an ADDRESS environment with
  `Interpreter.add_command_environment` while Rexx runs on another thread of
  the same instance can corrupt the heap: register environments when the
  interpreter is created (`command_environments=`) or before other threads run Rexx.
- [#2112](https://sourceforge.net/p/oorexx/bugs/2112/) (open): resolving an
  ADDRESS environment missing from the instance's table, while other threads
  run Rexx, can register a freed handler. Environments registered through
  the bridge are in the table; addressing one before it is registered is the
  risky case.

Not a bug, but worth knowing when writing native code: `IsString` is false
for strings the interpreter keeps in another form (internal integers, number
strings, subclasses of `.String`). It promises only that the object "may be
safely cast to a RexxStringObject". The bridge uses `IsOfType(o, "String")`
and `ObjectToString` for "is a string value".
