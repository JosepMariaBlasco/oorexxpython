/* edges.rex: the conversion probe.  How values cross the bridge, both ways,
 * one line each; tests/python/t80_conversion_probe.py compares its output
 * with edges.out.  A deliberate change in a conversion updates edges.out in
 * the same commit.  The ooRexx bridges for .NET, Lua and JavaScript have
 * probes of their own, and the family compares the results.
 *
 * Licensed under the Apache License, Version 2.0.
 */
k = .py~eval("lambda x: type(x).__name__ + ':' + repr(x)")
list = '0|-0|42|-7|007|+5| 5|5 |3.14|.5|5.|1E3|1e3|1E+3|1E-3|-1.5E2|1.0|-0.0|12345678901234567890123456789|3.14159265358979323846264|1E999|-1E999|1E-999|.|-|E3|1E|0x10|1_000|inf|NaN|abc||1 2'
do while list \= ""
  parse var list v "|" list
  say left("'"v"'", 32) k~new(v)
end
say left("12345678901234567890 * 10 (DIGITS 9 result)", 32) k~new(12345678901234567890 * 10)
numeric digits 30
say left("2**100 under DIGITS 30", 32) k~new(2**100)
numeric digits 9
say left(".true / .false", 32) k~new(.true) k~new(.false)
say left(".LogicalWrapper~true / ~false", 32) k~new(.LogicalWrapper~true) k~new(.LogicalWrapper~false)
say left("an Array of ~true and 1", 32) k~new(.array~of(.LogicalWrapper~true, 1))
say left(".json~true (JSONBoolean)", 32) k~new(.json~true)
say left(".nil", 32) k~new(.nil)
a = .array~new(4); a[1] = 1; a[3] = "x"
say left("sparse Array [1,,x,]", 32) k~new(a)
m = .array~new(2,2); m[1,1] = 1; m[2,2] = 4
say left("2-D Array 2x2", 32) k~new(m)
m = .array~new(2,3); do i = 1 to 2; do j = 1 to 3; m[i,j] = i * 10 + j; end; end
say left("2-D Array 2x3, m[i,j] = ij", 32) k~new(m)
m = .array~new(2,2,2); m[1,2,1] = "a"; m[2,1,2] = "b"
say left("3-D Array 2x2x2 [1,2,1] [2,1,2]", 32) k~new(m)
say left("nested ((1,2),(3,'a'))", 32) k~new(((1,2),(3,"a")))
a = .array~of(1, 2); a~append(a)
say left("a~append(a)", 32) k~new(a) .py~eval("lambda x: x[2] is x")~new(a)
b = .array~of(1); c = .array~of(2, b); b~append(c)
say left("b = (1, c), c = (2, b)", 32) k~new(b) .py~eval("lambda x: x[1][1] is x")~new(b)
x = .array~of(9)
say left(".array~of(x, x)", 32) k~new(.array~of(x, x)) .py~eval("lambda x: x[0] is x[1]")~new(.array~of(x, x))
t = 0; do 1000; t = .array~of(t); end
say left("Arrays nested 1000 deep", 32) .py~eval("lambda x: 'ok'")~new(t)
t = .array~of(t)
signal on syntax name tooDeep
say left("Arrays nested 1001 deep", 32) .py~eval("lambda x: 'ok'")~new(t)
tooDeep:
signal off syntax
if symbol("RC") == "VAR" then say left("Arrays nested 1001 deep", 32) "SYNTAX" condition("o")~code condition("o")~message
s.1 = "a"; s.0 = 1
say left("stem s.", 32) k~new(s.)
say left(".directory~new", 32) k~new(.directory~new)
say left(".stringtable~new", 32) k~new(.stringtable~new)
say left(".list~of(1,2)", 32) k~new(.list~of(1,2))
say left(".mutablebuffer~new('12')", 32) k~new(.mutablebuffer~new("12"))
say left("'é' (UTF-8 bytes)", 32) k~new("é")
say left("'ff'x (not UTF-8)", 32) k~new("ff"x)
say "--- Python -> Rexx"
e = "True|False|None|0|-0.0|10**30|2**0.5|0.1+0.2|1e16|1e-7|1.0|float('inf')|float('nan')|1+2j|b'ab\xff'|'café'|''|[1,2]|(1,2)|{'a':1}|{1}|__import__('decimal').Decimal('1.10')|__import__('fractions').Fraction(3,4)|__import__('enum').IntEnum('E','A B').B"
do while e \= ""
  parse var e x "|" e
  r = .py~eval(x)
  if r == .nil then say left(x, 45) ".nil"
  else if r~isA(.PyObject) then say left(x, 45) "PyObject:" r
  else if r~isA(.LogicalWrapper) then say left(x, 45) ".LogicalWrapper~"word("false true", r + 1)
  else say left(x, 45) "'"r"'" "len" r~length
end
np = .py~import("numpy")
t = .py~eval("lambda x: x")
say left("'ff'x round trip to Python and back", 45) (t~new("ff"x) == "ff"x)
say left("float repr as Rexx number: 1e16 + 1", 45) .py~eval("1e16") + 1
say left("np.float64(2.5) / np.int64(7) / np.bool_(True) / np.float32(0.1)", 45) np~float64(2.5) np~int64(7) np~bool_(1) np~float32(0.1)
say "--- float text (oorexx-bridges topics/float-text.md)"
f = "1.0|1/3|0.1 + 0.2|1e16|1e21|1e-7|-0.0|float(2**53 + 1)|2**53 + 1|1.7976931348623157e308|5e-324|float('nan')|float('inf')|-float('inf')"
do while f \= ""
  parse var f x "|" f
  r = .py~eval(x)
  say left(x, 28) left("'"r"'", 26) "len" right(r~length, 2) " datatype N:" datatype(r, "N")
end
rule = .py~eval("lambda x: type(x).__name__ + ':' + repr(x)")
asfloat = .py~eval("lambda x: repr(float(x)) if isinstance(x, str) else '(not a str)'")
do s over "1E+16", "1e16", "1E16", " 1.5 ", "-0", "-0.0", "NaN", "nan", "Infinity", "inf", "+infinity", "-infinity", "1E+999"
  say left("'"s"'", 12) left("rule:" rule~new(s), 36) "float(str):" asFloat(s)
end
say "--- explicit float range (oorexx-bridges topics/explicit-float-range.md)"
do s over "1E+999", "-1E+999", "1E-999", "-1E-999", "5E-324", "1.7976931348623157E+308", "1.8E+308", "+infinity", "nan"
  say left("'"s"'", 27) left(".py~float:" explicit(".py~float(s)", s), 66) ".py~box('float'):" explicit(".py~box('float', s)", s)
end
exit

explicit: procedure                 -- an explicit float, or the error
  parse arg expr, s
  signal on syntax
  interpret "r = .py~repr("expr")"
  return r
syntax:
  return "SYNTAX" condition("o")~code condition("o")~message

asFloat: procedure expose asfloat   -- what float() makes of the Rexx string
  signal on syntax
  return asfloat~new(.py~box("str", arg(1)))
syntax:
  return condition("o")~message
::requires "python.cls"
::requires "logicalwrapper.cls"
::requires "json.cls"
