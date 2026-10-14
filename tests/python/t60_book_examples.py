"""t60: the books' examples run, and print what the books say they print.

Every <example> of the books in docs/books (DocBook) that holds a program
(<programlisting language="rexx"> or language="python") and a <screen> is
run, and its output compared with the screen: Rexx programs with the `rexx`
on the PATH (ORX_PYTHON, REXX_PATH and the library path as run.sh sets
them), Python programs with this Python (PYTHONPATH to the bridge).  An
example with condition="oorexx53" needs ooRexx 5.3.0 (several threads at
once) and is skipped on 5.2.0.

Run by tests/run.sh, as the other Python tests.
"""
import html, os, re, subprocess, sys, tempfile

checks = failures = 0
def check(description, got, expected):
    global checks, failures
    checks += 1
    if got == expected:
        print(f"ok {checks} - {description}")
    else:
        failures += 1
        print(f"not ok {checks} - {description}\n   got:\n{got}\n   expected:\n{expected}")

HERE = os.path.dirname(os.path.abspath(__file__))
BOOKS = os.path.join(HERE, "..", "..", "docs", "books")

EXAMPLE = re.compile(r'<example\b([^>]*)>(.*?)</example>', re.S)
LISTING = re.compile(r'<programlisting\s+language="(rexx|python)"[^>]*>(.*?)</programlisting>', re.S)
SCREEN = re.compile(r'<screen\b[^>]*>(.*?)</screen>', re.S)
ATTR = re.compile(r'(\w+)="([^"]*)"')

def text(xml):
    """The text of a listing: no markup, entities resolved."""
    return html.unescape(re.sub(r'<[^>]+>', '', xml))

def normal(output):
    lines = output.replace("\r\n", "\n").split("\n")
    return "\n".join(line.rstrip() for line in lines).strip("\n")

def oorexx52():
    try:
        v = subprocess.run(["rexx", "-v"], capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return False
    return re.search(r'Version 5\.2\.', v) is not None

old_oorexx = oorexx52()
examples = []
for book in sorted(os.listdir(BOOKS)):
    folder = os.path.join(BOOKS, book, "en-US")
    if not os.path.isdir(folder):
        continue
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".xml"):
            continue
        with open(os.path.join(folder, name), encoding="utf-8") as f:
            source = f.read()
        for m in EXAMPLE.finditer(source):
            attrs = dict(ATTR.findall(m.group(1)))
            listing = LISTING.search(m.group(2))
            screen = SCREEN.search(m.group(2))
            if listing and screen:
                examples.append((book, attrs.get("id", name), attrs.get("condition", ""),
                                 listing.group(1), text(listing.group(2)), text(screen.group(1))))

with tempfile.TemporaryDirectory() as work:
    for book, ident, condition, language, program, expected in examples:
        description = f"{book} {ident}"
        if condition == "oorexx53" and old_oorexx:
            print(f"ok - {description} # SKIP needs ooRexx 5.3.0")
            continue
        path = os.path.join(work, "example." + ("rex" if language == "rexx" else "py"))
        with open(path, "w", encoding="utf-8") as f:
            f.write(program + "\n")
        command = ["rexx", path] if language == "rexx" else [sys.executable, path]
        try:
            run = subprocess.run(command, cwd=work, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=120)
            got = normal(run.stdout + run.stderr)
        except subprocess.TimeoutExpired:
            got = "(timed out)"
        check(description, got, normal(expected))

print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
