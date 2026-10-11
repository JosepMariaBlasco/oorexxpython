/* check.rex: is the ooRexx/Python bridge installed?  ooRexx -> Python.
 *   rexx check.rex
 * Needs this folder on REXX_PATH (python.cls) and the library path (the
 * package, librexxpython / rexxpython.dll), and a Python: ORX_PYTHON, or the
 * python3 (Windows: python) on the PATH.
 */
sys = .py~import("sys")
say "ooRexx/Python: ooRexx" .rexxInfo~version "r".rexxInfo~revision"," -
    "Python" sys~version~word(1) "("sys~executable")"
nums = .py~list((3, 1, 2))
nums~sort
if nums~__str__ \== "[1, 2, 3]" | .py~len(nums) \= 3 then do
  say "ooRexx/Python: FAILED (got" nums~__str__")"
  exit 1
end

-- ooRexx looks for ::requires files in the folder of the program first, so
-- this program finds python.cls next to it even without REXX_PATH (or the
-- PATH); other programs (the samples, yours) would not.
cls = .File~new(.PyObject~package~name)~parent
parse source system . me
if same(cls, .File~new(me)~parent) then do
  found = .false
  do var over "REXX_PATH", "PATH"
    rest = value(var, , "ENVIRONMENT")
    do while rest \== "" & \found
      parse var rest dir (.File~pathSeparator) rest
      if dir \== "" then found = same(dir, cls)
    end
  end
  if \found then do
    if system~upper~abbrev("WIN") then set = "set"; else set = "export"
    say "ooRexx/Python: NOT ON REXX_PATH: python.cls found only because check.rex is next to it;"
    say "ooRexx/Python: other programs (the samples) will not find it. Set REXX_PATH:"
    say "   " set "REXX_PATH="cls
    exit 1
  end
end
say "ooRexx/Python: OK"
exit 0

::routine same                          -- the same folder?
  use arg a, b
  a = .File~new(a)~absolutePath; b = .File~new(b)~absolutePath
  if .File~isCaseSensitive then return a == b
  return a~caselessEquals(b)

::requires "python.cls"
