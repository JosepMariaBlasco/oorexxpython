"""t15: Python threads calling one interpreter at once.

Kept apart from t10 because it needs ooRexx 5.3.0: on 5.2.0 concurrent
calls crash inside ooRexx (docs/design.md, *ooRexx versions*).
"""
import sys, threading
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

r = oorexx.Interpreter()
square = r.routine("return arg(1) ** 2")
results = []
def worker():
    acc = 0
    for i in range(500):
        acc += int(square(i))
    results.append(acc)
ts = [threading.Thread(target=worker) for _ in range(8)]
for t in ts: t.start()
for t in ts: t.join(60)
check("threads", results, [sum(i * i for i in range(500))] * 8)
r.close()
print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
