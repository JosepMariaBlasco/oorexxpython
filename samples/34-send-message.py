"""34-send-message.py: messages by name, as in BSF4ooRexx's RexxProxy.

o.name(...) is the short way; sendMessage(name, args) and sendMessage0 ...
sendMessage5 take the name as a string: one held in a variable, or one that
is no Python name ("[]=", "||").  sendMessageScope starts the method search
at a class, as Rexx's o~name:scope does.
"""
import oorexx

with oorexx.Interpreter() as rexx:
    square, circle = rexx.routine("""
      return .Square, .Circle
      ::class Shape
      ::method describe; return "a shape"
      ::class Square subclass Shape
      ::method describe; return "a square"
      ::method area;     use arg side; return side * side
      ::class Circle subclass Shape
      ::method describe; return "a circle"
      ::method area;     use arg r; return 3.14159 * r * r
    """)()
    s = square.new()
    message = "describe"                               # a name in a variable
    print(s.sendMessage(message), s.sendMessage1("area", 3), circle.new().sendMessage("area", [1]))
    print(s.sendMessageScope("describe", square.superclass()))     # Shape's describe

    d = rexx.eval(".directory~new")
    d.sendMessage2("[]=", "ooRexx", "lang")           # d["lang"] = "ooRexx"
    print(d.sendMessage1("[]", "lang"), d.sendMessage0("items"))

# Output:
# a square 9 3.14159
# a shape
# ooRexx 1
