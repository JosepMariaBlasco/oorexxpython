"""t40: messages by name from Python, as BSF4ooRexx's RexxProxy: sendMessage,
sendMessageScope, sendMessage0 ... sendMessage5."""
import sys
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

def raises(f):
    try:
        f()
    except Exception as e:
        return f"{type(e).__name__}: {e}"
    return "no exception"

r = oorexx.Interpreter()
r.routine("""
  .environment~orxpy.t40 = .Sub
  ::class Base public
  ::method hello;  return "base hello"
  ::method five;   use arg a, b, c, d, e; return a || b || c || d || e
  ::method sendMessage; return "a Rexx method"
  ::class Sub subclass Base public
  ::method hello;  return "sub hello"
""")()
Sub = r.eval(".orxpy.t40")
Base = Sub.superclass()
o = Sub.new()

# sendMessage(name[, args])
d = r.eval(".directory~new")
check("sendMessage, a name no Python name can be", d.sendMessage("[]=", [42, "x"]), None)
check("sendMessage, list",        d.sendMessage("[]", ["x"]), 42)
check("sendMessage, tuple",       d.sendMessage("hasIndex", ("x",)), 1)
check("sendMessage, no args",     d.sendMessage("items"), 1)
check("sendMessage, None",        d.sendMessage("ITEMS", None), 1)
name = "hello"
check("sendMessage, name in a variable", o.sendMessage(name), "sub hello")
check("sendMessage, operator",    r.eval(".mutablebuffer~new('ab')").sendMessage("||", ["c"]), "abc")

# sendMessage0 ... sendMessage5
b = r.eval(".mutablebuffer~new('ab')")
check("sendMessage0",             b.sendMessage0("length"), 2)
check("sendMessage1",             b.sendMessage1("append", "c").sendMessage0("string"), "abc")
check("sendMessage2",             b.sendMessage2("substr", 2, 1), "b")
check("sendMessage3",             str(b.sendMessage3("insert", "-", 1, 1)), "a-bc")
check("sendMessage4",             r.eval(".mutablebuffer~new('ab')").sendMessage4("insert", "XY", 1, 2, "-").sendMessage0("string"), "aXYb")
check("sendMessage5",             o.sendMessage5("five", 1, 2, 3, 4, 5), 12345)
check("sendMessageN, arity",      raises(lambda: o.sendMessage2("five", 1)),
      "TypeError: sendMessage2 takes 3 arguments, the message name and 2 more (2 given)")

# sendMessageScope(name, scope[, args])
check("plain message",            o.hello(), "sub hello")
check("sendMessageScope, superclass", o.sendMessageScope("hello", Base), "base hello")
check("sendMessageScope, own class",  o.sendMessageScope("HELLO", Sub, []), "sub hello")
check("sendMessageScope, args",   o.sendMessageScope("five", Base, (1, 2, 3, 4, 5)), 12345)
check("sendMessageScope, not a proxy", raises(lambda: o.sendMessageScope("hello", "Base")),
      "TypeError: sendMessageScope: the scope must be a Rexx class (an oorexx.RexxObject)")
check("sendMessageScope, not a class", raises(lambda: o.sendMessageScope("hello", d)),
      "RexxError: sendMessageScope: the scope must be a Rexx class")

# errors, and the names that are ours
check("name not a str",           raises(lambda: d.sendMessage(1)),
      "TypeError: sendMessage: the message name must be a str")
check("args not a sequence",      raises(lambda: d.sendMessage("x", 3)),
      "TypeError: sendMessage: the arguments must be a list or a tuple")
check("Rexx error",               raises(lambda: b.sendMessage("noSuchMethod")).startswith("RexxError: Rexx error 97.1"), True)
check("o.sendMessage is ours",    o.sendMessage.__name__, "sendMessage")
check("a Rexx SENDMESSAGE, by name", o.sendMessage("sendMessage"), "a Rexx method")

r.close()
print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
