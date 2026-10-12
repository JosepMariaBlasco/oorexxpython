/* 24-subclass.rex: Python classes with methods in Rexx.
 *
 * .py~extend(base, .RexxClass) makes a real Python subclass of base whose
 * methods are the Rexx class's: Python calls them (here threading.Thread's
 * start calls run, and list(), len(), "in" and reversed() call `__getitem__`
 * and `__len__`).  Each method gets a last argument, a StringTable: SELF is
 * the Python object, SUPER the base class (super() in Python), KWARGS the
 * keyword arguments.  .py~raise raises a Python exception: IndexError ends
 * a sequence.
 *
 * Python:
 *   class Worker(threading.Thread):
 *       def run(self): print("hello from", self.name)
 *   w = Worker(name="worker"); w.start(); w.join()
 *   class Squares(collections.abc.Sequence):
 *       def __init__(self, n): self.n = n
 *       def __getitem__(self, i):
 *           if i >= self.n: raise IndexError(i)
 *           return i * i
 *       def __len__(self): return self.n
 *   sq = Squares(5)
 *   print(list(sq), len(sq), 16 in sq, list(reversed(sq)))
 */
Worker = .py~extend("threading.Thread", .Worker)
w = Worker~new(.py~kw("name", "worker"))
w~start; w~join

Squares = .py~extend("collections.abc.Sequence", .Squares)
sq = Squares~new(5)
say .py~list(sq) .py~len(sq) .py~eval("lambda s: 16 in s")~new(sq) .py~list(.py~reversed(sq))

::class Worker
::method run
  use arg slot
  say "hello from" slot~self~name

::class Squares
::method __init__
  expose n
  use arg n, slot
  slot~super~__init__
::method __getitem__
  expose n
  use arg i, slot
  if i >= n then .py~raise(.py~IndexError, i)
  return i * i
::method __len__
  expose n
  return n

::requires "python.cls"
/* Output:
hello from worker
[0, 1, 4, 9, 16] 5 1 [16, 9, 4, 1, 0]
*/
