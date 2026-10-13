/* 24-subclass.rex: Rexx classes that extend Python classes.
 *
 * A subclass of PyObject extends a Python class with .py~extend, once, in
 * its ACTIVATE class method; its methods override the base's, and Python
 * calls them (here threading.Thread's start calls run, and list(), len(),
 * "in" and reversed() call __getitem__ and __len__).  INIT passes the
 * constructor's arguments to the base with self~init:super(...);
 * self~name:super(...) is the base's own member.  .py~raise raises a
 * Python exception: IndexError ends a sequence.
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
w = .Worker~new(.py~kw("name", "worker"))
w~start; w~join

sq = .Squares~new(5)
say .py~list(sq) .py~len(sq) .py~eval("lambda s: 16 in s")~new(sq) .py~list(.py~reversed(sq))

::class Worker subclass PyObject
::method activate class
  .py~extend(self, "threading.Thread")
::method run
  say "hello from" self~name

::class Squares subclass PyObject
::method activate class
  .py~extend(self, "collections.abc.Sequence")
::method init
  expose n
  use arg n
  self~init:super
::method __getitem__
  expose n
  use arg i
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
