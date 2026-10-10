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
say "ooRexx/Python: OK"

::requires "python.cls"
