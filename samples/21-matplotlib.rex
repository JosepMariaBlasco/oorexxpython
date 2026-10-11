/* 21-matplotlib.rex: a chart with matplotlib, saved as a PNG file.
 *
 * Python:
 *   import matplotlib; matplotlib.use("Agg")      # no screen needed
 *   import matplotlib.pyplot as plt, numpy as np
 *   x = np.linspace(0, 2 * np.pi, 100)
 *   plt.plot(x, np.sin(x), label="sin"); plt.plot(x, np.cos(x), label="cos")
 *   plt.legend(); plt.title("ooRexx draws"); plt.savefig("waves.png")
 */
.py~import("matplotlib")~use("Agg")
plt = .py~import("matplotlib.pyplot")
np  = .py~import("numpy")
x = np~linspace(0, 2 * np~pi, 100)
plt~plot(x, np~sin(x), .py~kw("label", "sin"))
plt~plot(x, np~cos(x), .py~kw("label", "cos"))
plt~legend
plt~title("ooRexx draws")
plt~savefig("waves.png")
say "waves.png:" (stream("waves.png", "c", "query size") > 10000)

::requires "python.cls"
/* Output:
waves.png: 1
*/
