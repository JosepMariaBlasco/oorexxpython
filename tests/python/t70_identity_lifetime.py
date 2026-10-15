"""t70: identity and lifetime of Python objects held by Rexx, and Python's
authority over types.

What these checks ask for comes from Tom Dyer's qualification probes for his
ooRexx/Python proof of concept in Alchemy (github.com/animatedads/alchemy,
packages/oorexx_python_macrospace_poc: python_object_lifetime_torture,
python_class_handle_lifetime_torture, returned_proxy_lifetime_torture,
rexx_held_proxy_lifetime_torture, python_type_authority_torture,
registry_rollback_torture), contributed by him under the Apache License 2.0.
His probes test his bridge's explicit handles and retain counts; here each
property is asked of this bridge's own model, where Rexx's and Python's
collectors decide (docs/design.md, *Lifetime*):

1. one Python object, passed to Rexx twice, is one object there, and it
   lives while Rexx holds it, however Python lets go of it;
2. a class, and its bases, as Rexx sees them, are the classes themselves,
   and live while Rexx holds them;
3. an object a Python call returns to Rexx lives while Rexx holds it, after
   everything that made it is gone;
4. a proxy held only in a Rexx collection keeps its object, and its
   identity;
5. Python decides isinstance and issubclass: abstract base classes,
   metaclasses, classes made at run time, and two classes with the same name;
6. calls that fail keep no reference to their arguments.
"""
import abc, gc, sys, weakref
import oorexx

checks = failures = 0
def check(description, got, expected):
    global checks, failures
    checks += 1
    if got == expected:
        print(f"ok {checks} - {description}")
    else:
        failures += 1
        print(f"not ok {checks} - {description}\n   got:      {got!r}\n   expected: {expected!r}")

CHURN = "do 20000; x = .array~new(200); end; return 0"   # forces collections

def settle(r):
    """collections (uninits run), then a call (the bridge drops what they queued)"""
    churn = r.routine(CHURN)
    for _ in range(3):
        churn()
        gc.collect()

class Target:
    def ping(self): return "alive"

r = oorexx.Interpreter()
keep = r.routine("use arg name, o; .local[name] = o; return 0")
fetch = r.routine("use arg name; return .local[name]")
drop = r.routine("use arg name; .local~remove(name); return 0")
same = r.routine("use arg a, b; return a == b")
send = r.routine("use arg o, message; return o~send(message)")

# 1. One object, twice, and its lifetime while Rexx holds it
o = Target()
w = weakref.ref(o)
check("the same object twice is the same object in Rexx", same(o, o), 1)
keep("T1", o)
keep("T1B", o)
del o
settle(r)
check("alive while Rexx holds it", w() is not None, True)
check("Rexx gives back the object itself", fetch("T1") is w(), True)
drop("T1")
settle(r)
check("alive while Rexx holds it once more", w() is not None, True)
drop("T1B")
settle(r)
check("released when Rexx holds it no more", w() is None, True)

# 2. Classes and their bases
class A: pass
class B: pass
C = type("C", (A, B), {})               # a class only Rexx will hold
o = C()
wc = weakref.ref(C)
classOf = r.routine("use arg o; return .py~type(o)")
basesOf = r.routine("use arg c; return c~getattr('__bases__')")
check("the class, asked twice, is the same class", same(classOf(o), classOf(o)), 1)
check("it is Python's class", classOf(o) is C, True)
check("its bases are Python's", basesOf(C), (A, B))
check("Python's issubclass through Rexx",
      r.routine("use arg c, b; return .py~issubclass(c, b)")(C, B), 1)
keep("C2", classOf(o))
del o, C
settle(r)
check("the class lives while Rexx holds it", wc() is not None, True)
check("an instance of it, made from Rexx",
      r.routine("return .py~type(.local['C2']~new)~getattr('__name__')")(), "C")
drop("C2")
settle(r)
check("the class is released when Rexx holds it no more", wc() is None, True)

# 3. An object a call returns, after what made it is gone
class Child:
    def ping(self): return "child alive"
class Parent:
    def __init__(self, child): self.child = child
    def childobj(self): return self.child
child = Child()
wchild = weakref.ref(child)
parent = Parent(child)
wparent = weakref.ref(parent)
r.routine("use arg p; .local['CHILD'] = p~childobj; return 0")(parent)
del child, parent
settle(r)
check("the parent is released", wparent() is None, True)
check("the returned child lives while Rexx holds it", wchild() is not None, True)
check("and can be used from Rexx", r.routine("return .local['CHILD']~ping")(), "child alive")
drop("CHILD")
settle(r)
check("the child is released when Rexx holds it no more", wchild() is None, True)

# 4. A proxy held only by a Rexx collection
o = Target()
w = weakref.ref(o)
r.routine("use arg o; .local['BAG'] = .array~of(o, 'text', o); return 0")(o)
del o
settle(r)
check("a Rexx Array keeps it", w() is not None, True)
check("calls through it",
      r.routine("return .local['BAG'][1]~ping .local['BAG'][3]~ping")(), "alive alive")
check("its identity in Rexx", r.routine("return .local['BAG'][1] == .local['BAG'][3]")(), 1)
check("its identity back in Python", fetch("BAG")[0] is w(), True)
drop("BAG")
settle(r)
check("released with the collection", w() is None, True)

# 5. Python's authority over types
class Animal(metaclass=abc.ABCMeta): pass
class Concrete: pass
Animal.register(Concrete)              # a virtual subclass: not in the MRO
class Meta(type): pass
class MetaAnimal(metaclass=Meta): pass
Dynamic = type("Dynamic", (MetaAnimal,), {})
def make():
    class SameName: pass
    return SameName
S1, S2 = make(), make()
isinstance_ = r.routine("use arg o, c; return .py~isinstance(o, c)")
issubclass_ = r.routine("use arg a, b; return .py~issubclass(a, b)")
check("an abstract base class's virtual subclass", isinstance_(Concrete(), Animal), 1)
check("its issubclass", issubclass_(Concrete, Animal), 1)
check("not in the MRO", Animal in Concrete.__mro__, False)
check("a class made at run time, with a metaclass", isinstance_(Dynamic(), MetaAnimal), 1)
check("its metaclass", r.routine("use arg c; return .py~type(c)")(Dynamic) is Meta, True)
check("two classes with the same name are two classes", same(S1, S2), 0)
check("same module and name", (S1.__module__, S1.__qualname__) == (S2.__module__, S2.__qualname__), True)
check("isinstance tells them apart", (isinstance_(S1(), S2), isinstance_(S2(), S1)), (0, 0))

# 6. Calls that fail keep no reference to their arguments
o = Target()
base = sys.getrefcount(o)
fail = r.routine("""
    use arg o, f
    signal on syntax
    x = f~new(o, o, o)                  -- a TypeError: too many arguments
    exit 1
  syntax:
    signal on syntax name again
    x = o~noSuchAttribute               -- an AttributeError
    exit 2
  again:
    return 0
""")
f = lambda x: x
for _ in range(100):
    fail(o, f)
settle(r)
check("100 failed calls leak no reference", sys.getrefcount(o) - base, 0)

r.close()
print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
