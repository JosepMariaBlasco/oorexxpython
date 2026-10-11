/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * bridge.cpp: oorexx.abi3.so, the ooRexx/Python bridge.
 *
 * Built against Python's Stable ABI only (Py_LIMITED_API = 3.9) and ooRexx's
 * public native API only (docs/design.md, *Python versions*).  Not linked against libpython: the
 * Py* symbols come from the process (python itself, or the libpython that
 * librexxpython.so loaded globally: docs/design.md, *Two libraries*).
 *
 * This file: Rexx -> Python (python.cls, .py, .PyObject).
 *
 * Locks (docs/design.md, *Threads and locks*).  Every native method works in three phases:
 *   1. Rexx arguments -> Val (ooRexx API only; may run Rexx code; no GIL);
 *   2. with the GIL: Val -> Python objects, the Python work, the result ->
 *      Val (Python API only);
 *   3. Val -> Rexx result, or the Python error raised as a Rexx condition
 *      (ooRexx API only; may run Rexx code, e.g. .PyObject~new; no GIL).
 * So no thread ever waits for the GIL while it runs Rexx code, and ooRexx
 * itself releases its kernel lock while native code runs.
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <Python.h>
#include "oorexxapi.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define ORXPY_EXPORT __declspec(dllexport)
static void setEnv(const char *name, const char *value) { _putenv_s(name, value); }
static void unsetEnv(const char *name) { _putenv_s(name, ""); }
#else
#include <dlfcn.h>
#define ORXPY_EXPORT __attribute__((visibility("default")))
static void setEnv(const char *name, const char *value) { setenv(name, value, 1); }
static void unsetEnv(const char *name) { unsetenv(name); }
#endif
#include <ctype.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <functional>

// debugging counters (ORXPY_DEBUG=1 prints them when the package unloads)
static std::atomic<long> statGlobalRefs(0), statReleased(0), statProxies(0), statProxiesFreed(0);

/* ------------------------------------------------------------------------ */
/* Python start-up when Rexx starts (the loader calls orxpy_launch)           */
/* ------------------------------------------------------------------------ */

static bool launchPending = false;
static std::string launchHome, launchExecutable;
static std::vector<std::string> launchPath;
static bool pythonOwned = false;          // we initialized Python (Rexx started)

extern "C" ORXPY_EXPORT
void orxpy_launch(const char *home, const char *executable, const char *const *path, int npath)
{
    launchPending = true;
    launchHome = home;
    launchExecutable = executable;
    launchPath.assign(path, path + npath);
}

// a Python str from bytes (UTF-8; undecodable bytes survive the round trip)
static PyObject *pyText(const char *s, size_t n)
{
    return PyUnicode_DecodeUTF8(s, (Py_ssize_t)n, "surrogateescape");
}

static void setSysAttr(const char *name, PyObject *value)
{
    PyObject *sys = PyImport_ImportModule("sys");
    if (sys != NULL && value != NULL) PyObject_SetAttrString(sys, name, value);
    Py_XDECREF(value);
    Py_XDECREF(sys);
    PyErr_Clear();
}

static void registerModule();

// called with no lock held, when ooRexx loads the package
static void startPython()
{
    if (!launchPending || Py_IsInitialized()) return;
    launchPending = false;

    // PYTHONHOME only for the initialization (the process' children, e.g.
    // ADDRESS SYSTEM "python3 ...", must not inherit it)
    const char *old = getenv("PYTHONHOME");
    std::string saved = old != NULL ? old : "";
    setEnv("PYTHONHOME", launchHome.c_str());
    Py_InitializeEx(0);                   // no signal handlers: ooRexx owns them
    if (old != NULL) setEnv("PYTHONHOME", saved.c_str()); else unsetEnv("PYTHONHOME");
    pythonOwned = true;

    // the executable's own sys.path (site-packages, a virtual environment's,
    // and '' first: modules next to the program can be imported)
    PyObject *path = PyList_New(0);
    for (const std::string &p : launchPath)
    {
        PyObject *item = pyText(p.data(), p.size());
        PyList_Append(path, item);
        Py_DECREF(item);
    }
    setSysAttr("path", path);
    setSysAttr("executable", pyText(launchExecutable.data(), launchExecutable.size()));
    PyObject *argv = PyList_New(1);
    PyList_SetItem(argv, 0, PyUnicode_FromString(""));
    setSysAttr("argv", argv);

    // Rexx SAY and Python print share the terminal: Python's lines must not
    // wait in a buffer (and Python is never finalized, see the unloader)
    PyObject *sys = PyImport_ImportModule("sys");
    for (const char *stream : {"stdout", "stderr"})
    {
        PyObject *s = sys != NULL ? PyObject_GetAttrString(sys, stream) : NULL;
        PyObject *r = s != NULL ? PyObject_GetAttrString(s, "reconfigure") : NULL;
        if (r != NULL)
        {
            PyObject *args = PyTuple_New(0);
            PyObject *kw = Py_BuildValue("{s:O}", "line_buffering", Py_True);
            Py_XDECREF(PyObject_Call(r, args, kw));
            Py_XDECREF(kw);
            Py_DECREF(args);
        }
        Py_XDECREF(r);
        Py_XDECREF(s);
        PyErr_Clear();
    }
    Py_XDECREF(sys);

    registerModule();
    PyEval_SaveThread();                  // from now on, the GIL is taken per call
}

/* ------------------------------------------------------------------------ */
/* The GIL                                                                    */
/* ------------------------------------------------------------------------ */

class Gil
{
  public:
    Gil() { state = PyGILState_Ensure(); }
    ~Gil() { PyGILState_Release(state); }
  private:
    PyGILState_STATE state;
};

/* ------------------------------------------------------------------------ */
/* Val: a value between the two runtimes, owned by neither lock               */
/* ------------------------------------------------------------------------ */

struct Val
{
    enum Kind { NIL, STR, REF, LIST, REXX } kind = NIL;
    std::string s;                        // STR: the bytes
    PyObject *ref = NULL;                 // REF: a Python object
                                          // REXX: its Python proxy, if it came from Python
    bool owned = false;                   // REF/REXX: we hold a reference to ref
    std::vector<Val> items;               // LIST
    RexxObjectPtr robj = NULLOBJECT;      // REXX: a Rexx object
    struct Inst *inst = NULL;             // REXX: its interpreter instance
    unsigned long keepKey = 0;            // REXX: robj is kept alive under this key
};

// a Python error, captured with the GIL and raised in Rexx without it
struct PyError
{
    bool set = false;
    std::string message;                  // "ValueError: ..."
    std::string traceback;
};

// what the bridge needs from the Rexx side during one call
struct RexxSide
{
    RexxThreadContext *tc;
    RexxInstance *inst;
    RexxClassObject pyObjectClass;
    RexxClassObject kwClass;
    RexxDirectoryObject keep = NULLOBJECT;
};

/*
 * Per interpreter instance: python.cls's classes (found by the first native
 * method that runs, with FindContextClass; used by threads that only have a
 * thread context: callbacks from Python), and the Directory that keeps alive
 * the Rexx objects Python holds.
 *
 * Why not RequestGlobalReference/ReleaseGlobalReference: in ooRexx 5.x (since
 * r10757, still in trunk r13254) InterpreterInstance::removeGlobalReference
 * calls memoryObject.addGlobalReference, so a release adds a reference and
 * nothing is ever freed (ooRexx bug #2100, fixed in r13257; see
 * notes/oorexx-bugs.md).  A Rexx object Python holds is put in the
 * instance's "keep" Directory under a key of its own, and removed when Python
 * drops it.  The classes and the
 * Directory themselves are global references, once per instance (never
 * released, so the bug does not matter for them).
 */
struct InstanceData
{
    RexxClassObject pyObjectClass, kwClass;
    RexxDirectoryObject keep;
    RexxObjectPtr graveyard;              // .PyObject~graveyard (see drainGraveyard)
};
static std::mutex instancesLock;
static std::map<RexxInstance *, InstanceData> instances;
static std::atomic<unsigned long> keepCounter(0);

static void drainReleases(RexxThreadContext *tc);

/*
 * An interpreter instance as the bridge's proxies see it.  ooRexx reuses the
 * address of a terminated instance for a later one, so a proxy cannot tell
 * by the RexxInstance pointer alone whether its instance is still the one it
 * was made in: it holds one of these instead, never freed (a few bytes per
 * instance), and closed once its instance is terminated.
 */
struct Inst
{
    RexxInstance *rexx;
    std::atomic<bool> closed;
    explicit Inst(RexxInstance *r) : rexx(r), closed(false) {}
};
static std::mutex handlesLock;
static std::map<RexxInstance *, Inst *> handles;   // the live ones

static Inst *handleOf(RexxInstance *inst)
{
    std::lock_guard<std::mutex> guard(handlesLock);
    Inst *&h = handles[inst];
    if (h == NULL) h = new Inst(inst);
    return h;
}

// a newly created instance: a new handle, even if its address is reused
static Inst *newHandle(RexxInstance *inst)
{
    std::lock_guard<std::mutex> guard(handlesLock);
    Inst *&h = handles[inst];
    if (h != NULL) h->closed = true;
    h = new Inst(inst);
    return h;
}

static bool isClosed(Inst *h) { return h == NULL || h->closed; }
static void drainGraveyard(RexxThreadContext *tc, RexxObjectPtr graveyard);

static RexxObjectPtr graveyardOf(RexxThreadContext *tc, RexxClassObject pyObjectClass)
{
    RexxObjectPtr q = tc->SendMessage0(pyObjectClass, "GRAVEYARD");
    return q == NULLOBJECT ? NULLOBJECT : tc->RequestGlobalReference(q);
}

static bool findClasses(RexxMethodContext *context, RexxSide &side)
{
    side.tc = context->threadContext;
    side.inst = context->threadContext->instance;
    side.pyObjectClass = context->FindContextClass("PYOBJECT");
    side.kwClass = context->FindContextClass("PYKWARGS");
    if (side.pyObjectClass == NULLOBJECT || side.kwClass == NULLOBJECT)
    {
        context->RaiseException1(Rexx_Error_Execution_user_defined,
            context->String("ooRexx/Python: python.cls classes not found"));
        return false;
    }
    RexxObjectPtr graveyard;
    {
        std::lock_guard<std::mutex> guard(instancesLock);
        auto it = instances.find(side.inst);
        if (it == instances.end())
        {
            InstanceData d;
            d.pyObjectClass = (RexxClassObject)side.tc->RequestGlobalReference(side.pyObjectClass);
            d.kwClass = (RexxClassObject)side.tc->RequestGlobalReference(side.kwClass);
            d.keep = (RexxDirectoryObject)side.tc->RequestGlobalReference(side.tc->NewDirectory());
            d.graveyard = graveyardOf(side.tc, side.pyObjectClass);
            instances[side.inst] = d;
            side.keep = d.keep;
            graveyard = d.graveyard;
        }
        else
        {
            side.keep = it->second.keep;
            graveyard = it->second.graveyard;
        }
    }
    drainReleases(side.tc);               // Rexx objects Python dropped since
    drainGraveyard(side.tc, graveyard);   // Python objects Rexx dropped since
    return true;
}

// the same for a thread context (an attached thread); false if python.cls
// was never loaded in that instance
static bool sideFor(RexxThreadContext *tc, RexxSide &side, RexxObjectPtr *graveyard = NULL)
{
    side.tc = tc;
    side.inst = tc->instance;
    std::lock_guard<std::mutex> guard(instancesLock);
    auto it = instances.find(side.inst);
    if (it == instances.end()) return false;
    side.pyObjectClass = it->second.pyObjectClass;
    side.kwClass = it->second.kwClass;
    side.keep = it->second.keep;
    if (graveyard != NULL) *graveyard = it->second.graveyard;
    return true;
}

static void keyText(unsigned long key, char *buf, size_t n)
{
    snprintf(buf, n, "%lx", key);
}

// keep o alive while Python holds it: its key (0 if impossible)
static unsigned long keepObject(RexxSide &side, RexxObjectPtr o)
{
    if (side.keep == NULLOBJECT) return 0;
    unsigned long key = ++keepCounter;
    char buf[32];
    keyText(key, buf, sizeof buf);
    side.tc->DirectoryPut(side.keep, o, buf);
    statGlobalRefs++;
    return key;
}

/*
 * A proxy dies under the GIL, where no Rexx call may wait for the kernel
 * lock: its release is queued here and done by the next thread that enters
 * that interpreter instance.
 */
static std::mutex releasesLock;
static std::vector<std::pair<Inst *, unsigned long>> pendingReleases;

static void queueRelease(Inst *inst, unsigned long key)
{
    if (key == 0) return;
    std::lock_guard<std::mutex> guard(releasesLock);
    pendingReleases.emplace_back(inst, key);
}

static void drainReleases(RexxThreadContext *tc)
{
    std::vector<unsigned long> mine;
    Inst *self = handleOf(tc->instance);
    {
        std::lock_guard<std::mutex> guard(releasesLock);
        for (size_t i = 0; i < pendingReleases.size(); )
        {
            if (pendingReleases[i].first == self)
            {
                mine.push_back(pendingReleases[i].second);
                pendingReleases[i] = pendingReleases.back();
                pendingReleases.pop_back();
            }
            else i++;
        }
    }
    if (mine.empty()) return;
    RexxDirectoryObject keep = NULLOBJECT;
    {
        std::lock_guard<std::mutex> guard(instancesLock);
        auto it = instances.find(tc->instance);
        if (it != instances.end()) keep = it->second.keep;
    }
    if (keep == NULLOBJECT) return;
    char buf[32];
    for (unsigned long key : mine)
    {
        keyText(key, buf, sizeof buf);
        tc->DirectoryRemove(keep, buf);
    }
    statReleased += (long)mine.size();
}

// the Python proxies of Rexx objects (defined below, "oorexx.RexxObject")
static PyObject *newRexxProxy(Inst *inst, RexxObjectPtr o, unsigned long key);
static bool rexxProxyTarget(PyObject *o, Inst **inst, RexxObjectPtr *target);

/* phase 1: Rexx -> Val (no GIL) ------------------------------------------ */

static void rexxToVal(RexxSide &side, RexxObjectPtr o, Val &v)
{
    RexxThreadContext *c = side.tc;
    if (o == NULLOBJECT || o == c->Nil())
    {
        v.kind = Val::NIL;
    }
    else if (c->IsString(o))
    {
        v.kind = Val::STR;
        v.s.assign(c->StringData((RexxStringObject)o), c->StringLength((RexxStringObject)o));
    }
    else if (c->IsOfType(o, "STRING"))
    {
        // a string the interpreter keeps in another form (the results of
        // arithmetic and of many built-in functions are internal integers,
        // which IsString does not recognize)
        RexxStringObject str = c->ObjectToString(o);
        v.kind = Val::STR;
        v.s.assign(c->StringData(str), c->StringLength(str));
    }
    else if (c->IsInstanceOf(o, side.pyObjectClass))
    {
        RexxObjectPtr p = c->SendMessage0(o, "PY.POINTER");
        v.kind = Val::REF;
        v.ref = p != NULLOBJECT ? (PyObject *)c->PointerValue((RexxPointerObject)p) : NULL;
        v.owned = false;                  // the proxy keeps it alive during the call
        if (v.ref == NULL) v.kind = Val::NIL;
    }
    else if (c->IsArray(o))
    {
        v.kind = Val::LIST;
        RexxArrayObject a = (RexxArrayObject)o;
        size_t n = c->ArraySize(a);
        v.items.resize(n);
        for (size_t i = 1; i <= n; i++) rexxToVal(side, c->ArrayAt(a, i), v.items[i - 1]);
    }
    else
    {
        // any other Rexx object goes by reference (an oorexx.RexxObject in
        // Python: callable, if it has a CALL method, as Routines do)
        v.kind = Val::REXX;
        v.robj = o;
        v.inst = handleOf(side.inst);
        v.keepKey = keepObject(side, o);
    }
}

// a method's arguments, with an optional .py~kw(...) marker last
struct Args
{
    std::vector<Val> positional;
    std::vector<std::pair<std::string, Val>> keywords;
    ~Args();                              // drops what the values still hold
};

static void rexxArgs(RexxSide &side, RexxArrayObject args, size_t first, Args &out)
{
    RexxThreadContext *c = side.tc;
    size_t n = args != NULLOBJECT ? c->ArraySize(args) : 0;
    size_t last = n;
    if (n >= first)
    {
        RexxObjectPtr tail = c->ArrayAt(args, n);
        if (tail != NULLOBJECT && c->IsInstanceOf(tail, side.kwClass))
        {
            last = n - 1;
            RexxArrayObject names = (RexxArrayObject)c->SendMessage0(tail, "NAMES");
            RexxArrayObject values = (RexxArrayObject)c->SendMessage0(tail, "VALUES");
            size_t k = c->ArraySize(names);
            for (size_t i = 1; i <= k; i++)
            {
                std::pair<std::string, Val> kv;
                kv.first = c->ObjectToStringValue(c->ArrayAt(names, i));
                rexxToVal(side, c->ArrayAt(values, i), kv.second);
                out.keywords.push_back(kv);
            }
        }
    }
    for (size_t i = first; i <= last; i++)
    {
        out.positional.emplace_back();
        rexxToVal(side, c->ArrayAt(args, i), out.positional.back());
    }
}

/* phase 2: Val <-> Python (with the GIL) ----------------------------------- */

static bool isDigits(const char *p, const char *end)
{
    if (p == end) return false;
    for (; p < end; p++) if (*p < '0' || *p > '9') return false;
    return true;
}

// what a Rexx string becomes in Python (docs/design.md, *Values*)
enum NumberKind { NOT_NUMBER, INTEGER_NUMBER, FLOAT_NUMBER };

static NumberKind numberKind(const std::string &s)
{
    const char *p = s.data(), *end = p + s.size();
    if (p < end && *p == '-') p++;
    if (p == end) return NOT_NUMBER;
    // canonical integer: 0, or no leading zero
    if (isDigits(p, end)) return (*p != '0' || end - p == 1) ? INTEGER_NUMBER : NOT_NUMBER;
    // a Rexx number with a decimal point and/or an exponent
    const char *e = p;
    while (e < end && *e != 'e' && *e != 'E') e++;
    const char *dot = (const char *)memchr(p, '.', e - p);
    bool mantissa;
    if (dot != NULL)
    {
        bool intPart = dot == p || isDigits(p, dot);
        bool fracPart = dot + 1 == e || isDigits(dot + 1, e);
        mantissa = intPart && fracPart && (e - p) > 1;      // not a lone "."
    }
    else
    {
        mantissa = isDigits(p, e);
    }
    if (!mantissa) return NOT_NUMBER;
    if (e == end) return dot != NULL ? FLOAT_NUMBER : NOT_NUMBER;   // "007": not a number
    const char *x = e + 1;
    if (x < end && (*x == '+' || *x == '-')) x++;
    return isDigits(x, end) ? FLOAT_NUMBER : NOT_NUMBER;
}

// a new reference, or NULL with a Python error set
static PyObject *valToPy(const Val &v)
{
    switch (v.kind)
    {
        case Val::NIL:
            Py_INCREF(Py_None);
            return Py_None;
        case Val::REF:
            Py_INCREF(v.ref);
            return v.ref;
        case Val::REXX:
        {
            if (v.ref == NULL)
            {
                // the proxy takes over the global reference; the Val keeps
                // the proxy, should it be converted again
                Val &w = const_cast<Val &>(v);
                w.ref = newRexxProxy(v.inst, v.robj, v.keepKey);
                if (w.ref == NULL) return NULL;
                w.owned = true;
                w.keepKey = 0;
            }
            Py_INCREF(v.ref);
            return v.ref;
        }
        case Val::LIST:
        {
            PyObject *list = PyList_New((Py_ssize_t)v.items.size());
            if (list == NULL) return NULL;
            for (size_t i = 0; i < v.items.size(); i++)
            {
                PyObject *item = valToPy(v.items[i]);
                if (item == NULL) { Py_DECREF(list); return NULL; }
                PyList_SetItem(list, (Py_ssize_t)i, item);
            }
            return list;
        }
        case Val::STR:
        default:
            switch (numberKind(v.s))
            {
                case INTEGER_NUMBER: return PyLong_FromString(v.s.c_str(), NULL, 10);
                case FLOAT_NUMBER:
                {
                    PyObject *text = PyUnicode_FromString(v.s.c_str());
                    if (text == NULL) return NULL;
                    PyObject *f = PyFloat_FromString(text);
                    Py_DECREF(text);
                    return f;
                }
                default: return pyText(v.s.data(), v.s.size());
            }
    }
}

// a numpy scalar (np.int64, np.float64, np.bool_...): its type comes from
// numpy and it is zero-dimensional
static bool isNumpyScalar(PyObject *o)
{
    PyObject *type = (PyObject *)Py_TYPE(o);
    PyObject *module = PyObject_GetAttrString(type, "__module__");
    bool numpy = false;
    if (module != NULL && PyUnicode_Check(module))
    {
        PyObject *bytes = PyUnicode_AsUTF8String(module);
        if (bytes != NULL)
        {
            numpy = strcmp(PyBytes_AsString(bytes), "numpy") == 0;
            Py_DECREF(bytes);
        }
    }
    Py_XDECREF(module);
    if (numpy)
    {
        PyObject *ndim = PyObject_GetAttrString(o, "ndim");
        numpy = ndim != NULL && PyLong_Check(ndim) && PyLong_AsLong(ndim) == 0;
        Py_XDECREF(ndim);
    }
    PyErr_Clear();
    return numpy;
}

// Python -> Val; steals the reference to o.  keep: always a proxy.
static void pyToVal(PyObject *o, Val &v, bool keep = false)
{
    if (o == Py_None && !keep)
    {
        v.kind = Val::NIL;
        Py_DECREF(o);
        return;
    }
    RexxObjectPtr target;
    Inst *inst;
    if (rexxProxyTarget(o, &inst, &target))
    {
        // a Rexx object coming back: itself (the proxy is kept until the
        // Rexx side has it)
        v.kind = Val::REXX;
        v.robj = target;
        v.inst = inst;
        v.ref = o;
        v.owned = true;
        return;
    }
    if (!keep)
    {
        PyObject *text = NULL;
        if (PyBool_Check(o))
        {
            v.kind = Val::STR;
            v.s = o == Py_True ? "1" : "0";
            Py_DECREF(o);
            return;
        }
        if (PyLong_Check(o))
        {
            // the plain int value (not an IntEnum's or other subclass' str)
            PyObject *plain = PyNumber_Long(o);
            text = plain != NULL ? PyObject_Str(plain) : NULL;
            Py_XDECREF(plain);
        }
        else if (PyFloat_Check(o))
        {
            // repr of the plain float: shortest round-trip digits (numpy 2's
            // float64 repr is "np.float64(2.0)")
            PyObject *plain = PyFloat_FromDouble(PyFloat_AsDouble(o));
            text = plain != NULL ? PyObject_Repr(plain) : NULL;
            Py_XDECREF(plain);
        }
        else if (isNumpyScalar(o))
        {
            // np.int64, np.bool_, np.float32...: its Python value (item())
            PyObject *item = PyObject_CallMethod(o, "item", NULL);
            if (item != NULL && !isNumpyScalar(item))
            {
                Py_DECREF(o);
                pyToVal(item, v, false);
                return;
            }
            Py_XDECREF(item);
            PyErr_Clear();
        }
        else if (PyUnicode_Check(o)) { text = o; Py_INCREF(text); }
        else if (PyBytes_Check(o))
        {
            char *data;
            Py_ssize_t n;
            PyBytes_AsStringAndSize(o, &data, &n);
            v.kind = Val::STR;
            v.s.assign(data, (size_t)n);
            Py_DECREF(o);
            return;
        }
        if (text != NULL)
        {
            PyObject *bytes = PyUnicode_AsEncodedString(text, "utf-8", "surrogateescape");
            Py_DECREF(text);
            if (bytes != NULL)
            {
                char *data;
                Py_ssize_t n;
                PyBytes_AsStringAndSize(bytes, &data, &n);
                v.kind = Val::STR;
                v.s.assign(data, (size_t)n);
                Py_DECREF(bytes);
                Py_DECREF(o);
                return;
            }
            PyErr_Clear();
        }
    }
    v.kind = Val::REF;
    v.ref = o;
    v.owned = true;
}

// drop what a Val still owns (with the GIL)
static void dropVal(Val &v)
{
    if ((v.kind == Val::REF || v.kind == Val::REXX) && v.owned && v.ref != NULL) Py_DECREF(v.ref);
    v.ref = NULL;
    v.owned = false;
    if (v.kind == Val::REXX && v.keepKey != 0) queueRelease(v.inst, v.keepKey);
    v.keepKey = 0;
    for (Val &i : v.items) dropVal(i);
}

static std::string pyString(PyObject *o)
{
    std::string out;
    PyObject *text = o != NULL ? PyObject_Str(o) : NULL;
    PyObject *bytes = text != NULL ? PyUnicode_AsEncodedString(text, "utf-8", "replace") : NULL;
    if (bytes != NULL)
    {
        char *data;
        Py_ssize_t n;
        PyBytes_AsStringAndSize(bytes, &data, &n);
        out.assign(data, (size_t)n);
    }
    Py_XDECREF(bytes);
    Py_XDECREF(text);
    PyErr_Clear();
    return out;
}

// take the pending Python exception into err
static void captureError(PyError &err)
{
    PyObject *type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb);
    if (type == NULL) return;
    PyErr_NormalizeException(&type, &value, &tb);
    err.set = true;
    PyObject *name = PyObject_GetAttrString(type, "__name__");
    std::string text = pyString(value);
    err.message = pyString(name) + (text.empty() ? "" : ": " + text);
    Py_XDECREF(name);
    PyObject *tbm = PyImport_ImportModule("traceback");
    PyObject *fmt = tbm != NULL ? PyObject_GetAttrString(tbm, "format_exception") : NULL;
    PyObject *lines = fmt != NULL ? PyObject_CallFunctionObjArgs(fmt, type,
        value != NULL ? value : Py_None, tb != NULL ? tb : Py_None, NULL) : NULL;
    if (lines != NULL)
    {
        PyObject *empty = PyUnicode_FromString("");
        PyObject *joined = PyUnicode_Join(empty, lines);
        err.traceback = pyString(joined);
        Py_XDECREF(joined);
        Py_XDECREF(empty);
    }
    Py_XDECREF(lines);
    Py_XDECREF(fmt);
    Py_XDECREF(tbm);
    PyErr_Clear();
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
}

// a bridge error (not raised by Python code), Python-style
static void bridgeError(PyError &err, const std::string &message)
{
    err.set = true;
    err.message = message;
}

// the positional arguments as a tuple and the keywords as a dict (or NULL)
static bool buildArgs(const Args &a, PyObject **tuple, PyObject **dict)
{
    *dict = NULL;
    *tuple = PyTuple_New((Py_ssize_t)a.positional.size());
    if (*tuple == NULL) return false;
    for (size_t i = 0; i < a.positional.size(); i++)
    {
        PyObject *item = valToPy(a.positional[i]);
        if (item == NULL) { Py_CLEAR(*tuple); return false; }
        PyTuple_SetItem(*tuple, (Py_ssize_t)i, item);
    }
    if (!a.keywords.empty())
    {
        *dict = PyDict_New();
        for (const auto &kv : a.keywords)
        {
            PyObject *value = valToPy(kv.second);
            if (value == NULL || PyDict_SetItemString(*dict, kv.first.c_str(), value) < 0)
            {
                Py_XDECREF(value);
                Py_CLEAR(*tuple);
                Py_CLEAR(*dict);
                return false;
            }
            Py_DECREF(value);
        }
    }
    return true;
}

// call f(*args, **kw): the result as a Val
static void callInto(PyObject *f, const Args &a, Val &result, PyError &err, bool keep = false)
{
    PyObject *tuple, *dict;
    if (!buildArgs(a, &tuple, &dict)) { captureError(err); return; }
    PyObject *r = PyObject_Call(f, tuple, dict);
    Py_DECREF(tuple);
    Py_XDECREF(dict);
    if (r == NULL) captureError(err); else pyToVal(r, result, keep);
}

static std::string lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

static std::string upper(const std::string &s)
{
    std::string out = s;
    for (char &c : out) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return out;
}

/*
 * The attribute of o a Rexx (uppercase) message name means (docs/design.md,
 * *Names*).
 * Found: true, *exact set.  Not found: false, no error.  Ambiguous or a
 * Python error: false, err set.
 *   - the all-lowercase spelling, if o has it (the common case, no scan);
 *   - else the names of dir(o) that match case-insensitively: one -> it;
 *     several -> the one starting in lowercase, if it is the only one.
 */
static bool resolveName(PyObject *o, const std::string &rexxName, std::string *exact, PyError &err)
{
    std::string low = lower(rexxName);
    PyObject *attr = PyObject_GetAttrString(o, low.c_str());
    if (attr != NULL)
    {
        Py_DECREF(attr);
        *exact = low;
        return true;
    }
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) { captureError(err); return false; }
    PyErr_Clear();

    PyObject *names = PyObject_Dir(o);
    if (names == NULL) { captureError(err); return false; }
    std::vector<std::string> matches;
    Py_ssize_t n = PyList_Size(names);
    for (Py_ssize_t i = 0; i < n; i++)
    {
        std::string name = pyString(PyList_GetItem(names, i));
        if (upper(name) == rexxName) matches.push_back(name);
    }
    Py_DECREF(names);
    if (matches.size() == 1) { *exact = matches[0]; return true; }
    if (matches.empty()) return false;
    std::vector<std::string> lowerFirst;
    for (const std::string &m : matches) if (m[0] >= 'a' && m[0] <= 'z') lowerFirst.push_back(m);
    if (lowerFirst.size() == 1) { *exact = lowerFirst[0]; return true; }
    std::string list;
    for (const std::string &m : matches) list += (list.empty() ? "" : ", ") + m;
    bridgeError(err, "ambiguous name " + rexxName + " (" + list +
                     "): use getattr, setattr or invoke with the exact name");
    return false;
}

/* phase 3: Val -> Rexx (no GIL) -------------------------------------------- */

static RexxObjectPtr valToRexx(RexxSide &side, Val &v)
{
    RexxThreadContext *c = side.tc;
    switch (v.kind)
    {
        case Val::STR:
            return c->String(v.s.data(), v.s.size());
        case Val::REF:
        {
            // the proxy takes over our reference (its UNINIT drops it)
            RexxObjectPtr proxy = c->SendMessage1(side.pyObjectClass, "NEW",
                                                  c->NewPointer(v.ref));
            v.owned = false;
            return proxy;
        }
        case Val::REXX:
            if (isClosed(v.inst))
            {
                // its instance let it go when it was closed: it may be gone
                c->RaiseException1(Rexx_Error_Execution_user_defined,
                    c->String("ooRexx/Python: a Rexx object of an interpreter instance that has been closed"));
                return NULLOBJECT;
            }
            return v.robj;
        case Val::LIST:
        {
            RexxArrayObject a = c->NewArray(v.items.size());
            for (size_t i = 0; i < v.items.size(); i++)
            {
                RexxObjectPtr item = valToRexx(side, v.items[i]);
                if (item != NULLOBJECT) c->ArrayPut(a, item, i + 1);
            }
            return a;
        }
        case Val::NIL:
        default:
            return c->Nil();
    }
}

// does v still hold Python references (to drop with the GIL)?
static bool holdsPython(const Val &v)
{
    if (v.owned && v.ref != NULL) return true;
    for (const Val &i : v.items) if (holdsPython(i)) return true;
    return false;
}

// does v hold Python objects borrowed from a .PyObject proxy (REF, not owned)?
static bool holdsBorrowed(const Val &v)
{
    if (v.kind == Val::REF && !v.owned && v.ref != NULL) return true;
    for (const Val &i : v.items) if (holdsBorrowed(i)) return true;
    return false;
}

// take a reference of our own to every borrowed Python object (with the GIL)
static void ownBorrowed(Val &v)
{
    if (v.kind == Val::REF && !v.owned && v.ref != NULL)
    {
        Py_INCREF(v.ref);
        v.owned = true;
    }
    for (Val &i : v.items) ownBorrowed(i);
}

// a call's arguments, after the call (taking the GIL only if needed; never
// called with the GIL held by this thread's caller code in a way that could
// wait for Rexx: PyGILState_Ensure nests)
Args::~Args()
{
    bool python = false;
    for (const Val &v : positional) python = python || holdsPython(v);
    for (const auto &kv : keywords) python = python || holdsPython(kv.second);
    if (python)
    {
        Gil gil;
        for (Val &v : positional) dropVal(v);
        for (auto &kv : keywords) dropVal(kv.second);
    }
    else
    {
        for (Val &v : positional) dropVal(v);
        for (auto &kv : keywords) dropVal(kv.second);
    }
}

static RexxObjectPtr finish(RexxSide &side, Val &result, PyError &err)
{
    RexxThreadContext *c = side.tc;
    if (err.set)
    {
        if (result.kind != Val::NIL) { Gil gil; dropVal(result); }
        RexxArrayObject extra = c->NewArray(2);
        std::string text = "Python error: " + err.message;
        if (!text.empty() && text.back() == '.') text.pop_back();   // the message adds one
        c->ArrayPut(extra, c->String(text.c_str()), 1);
        c->ArrayPut(extra, c->String(err.traceback.c_str()), 2);
        c->RaiseException(Rexx_Error_Execution_user_defined, extra);
        return NULLOBJECT;
    }
    RexxObjectPtr r = valToRexx(side, result);
    if (holdsPython(result)) { Gil gil; dropVal(result); }
    return r;
}

/* ------------------------------------------------------------------------ */
/* oorexx.RexxObject: a Rexx object in Python                                 */
/* ------------------------------------------------------------------------ */

/*
 *   r(args...)          r~call(args...)  (Routines are callable as they are)
 *   r.name(args...)     r~name(args...)  (names uppercased, as Rexx does)
 *   r[k] / r[k] = v     r~"[]"(k) / r~"[]="(v, k)
 *   str(r)              r~string
 *   iter(r), len(r)     r~makeArray / r~items
 *
 * Each call (docs/design.md, *Threads and locks*): Python arguments -> Val
 * with the GIL; then the GIL is released, the thread attaches to the Rexx instance (nesting, if it is a
 * Rexx thread already inside the bridge), sends the message, converts the
 * result, detaches; then, with the GIL again, Val -> Python.  A Rexx
 * condition becomes an oorexx.RexxError.
 */

struct RexxProxy
{
    PyObject_HEAD
    Inst *inst;
    RexxObjectPtr target;                 // kept alive in the instance's keep Directory
    unsigned long key;                    // under this key
};

struct RexxBound                          // r.name, waiting to be called
{
    PyObject_HEAD
    PyObject *proxy;
    char *message;
};

static PyObject *rexxProxyType = NULL;
static PyObject *rexxBoundType = NULL;
static PyObject *rexxErrorType = NULL;

static bool rexxProxyTarget(PyObject *o, Inst **inst, RexxObjectPtr *target)
{
    if (rexxProxyType == NULL || !PyObject_TypeCheck(o, (PyTypeObject *)rexxProxyType)) return false;
    *inst = ((RexxProxy *)o)->inst;
    *target = ((RexxProxy *)o)->target;
    return true;
}

static PyObject *newRexxProxy(Inst *inst, RexxObjectPtr o, unsigned long key)
{
    PyTypeObject *type = (PyTypeObject *)rexxProxyType;
    allocfunc alloc = (allocfunc)PyType_GetSlot(type, Py_tp_alloc);
    RexxProxy *p = (RexxProxy *)alloc(type, 0);
    if (p == NULL) return NULL;
    p->inst = inst;
    p->target = o;
    p->key = key;
    statProxies++;
    return (PyObject *)p;
}

static void rexxProxyDealloc(PyObject *self)
{
    RexxProxy *p = (RexxProxy *)self;
    queueRelease(p->inst, p->key);
    statProxiesFreed++;
    PyTypeObject *type = Py_TYPE(self);
    freefunc tpFree = (freefunc)PyType_GetSlot(type, Py_tp_free);
    tpFree(self);
    Py_DECREF(type);
}

// send message to target with args (a tuple); a new reference or NULL
/*
 * Interpreter instances closed from Python (oorexx.Interpreter.close): their
 * proxies may outlive them, and must not touch them.
 */

// a pending Rexx condition as the text of an oorexx.RexxError (and cleared)
static std::string takeCondition(RexxThreadContext *tc)
{
    RexxDirectoryObject cond = tc->GetConditionInfo();
    RexxCondition info;
    tc->DecodeConditionInfo(cond, &info);
    std::string name = info.conditionName != NULLOBJECT ? tc->CString(info.conditionName) : "SYNTAX";
    std::string text = info.message != NULLOBJECT ? tc->CString(info.message) : "";
    if (text.empty() && info.errortext != NULLOBJECT) text = tc->CString(info.errortext);
    tc->ClearCondition();
    if (name != "SYNTAX")
    {
        return "Rexx condition " + name + (text.empty() ? "" : ": " + text);
    }
    char code[64];
    snprintf(code, sizeof code, "%ld.%ld", (long)(info.code / 1000), (long)(info.code % 1000));
    return std::string("Rexx error ") + code + ": " + text;
}

/*
 * Python -> Rexx, the one way in (docs/design.md, *Threads and locks*): the
 * Python arguments (args from index skip on, with first before them if given) become Vals with the GIL;
 * the GIL is released; the thread attaches to the instance (nesting, if it is
 * a Rexx thread already inside the bridge); op does the Rexx work with the
 * arguments as a Rexx Array; its result becomes a Val; the thread detaches;
 * with the GIL again, the Val becomes the Python result.  A Rexx condition
 * becomes an oorexx.RexxError.  op returns false (and sets error) on failure.
 */
typedef std::function<RexxObjectPtr(RexxThreadContext *, RexxSide &, RexxArrayObject, std::string &)> RexxOp;

static PyObject *intoRexx(Inst *inst, PyObject *args, Py_ssize_t skip, PyObject *first, const RexxOp &op)
{
    if (isClosed(inst))
    {
        PyErr_SetString(rexxErrorType, "the Rexx interpreter instance has been closed");
        return NULL;
    }
    // with the GIL: the arguments
    Args a;
    if (first != NULL)
    {
        a.positional.emplace_back();
        Py_INCREF(first);
        pyToVal(first, a.positional.back());
    }
    Py_ssize_t n = args != NULL ? PyTuple_Size(args) : 0;
    for (Py_ssize_t i = skip; i < n; i++)
    {
        PyObject *item = PyTuple_GetItem(args, i);
        Py_INCREF(item);
        a.positional.emplace_back();
        pyToVal(item, a.positional.back());
    }

    // without the GIL: the Rexx work
    Val result;
    std::string error;
    PyThreadState *saved = PyEval_SaveThread();
    RexxThreadContext *tc;
    if (!inst->rexx->AttachThread(&tc))
    {
        error = "cannot attach this thread to the Rexx interpreter instance";
    }
    else
    {
        drainReleases(tc);
        RexxSide side;
        RexxObjectPtr graveyard = NULLOBJECT;
        if (!sideFor(tc, side, &graveyard))
        {
            side.tc = tc;
            side.inst = tc->instance;
            side.pyObjectClass = NULLOBJECT;
            side.kwClass = NULLOBJECT;
        }
        // Python objects Rexx dropped since: a program driven from Python may
        // never run a .PyObject method, which is where the graveyard is
        // otherwise drained (findClasses)
        drainGraveyard(tc, graveyard);
        RexxArrayObject rargs = tc->NewArray(a.positional.size());
        for (size_t i = 0; i < a.positional.size(); i++)
        {
            RexxObjectPtr o = valToRexx(side, a.positional[i]);
            if (o != NULLOBJECT) tc->ArrayPut(rargs, o, i + 1);
        }
        RexxObjectPtr r = tc->CheckCondition() ? NULLOBJECT : op(tc, side, rargs, error);
        if (tc->CheckCondition()) error = takeCondition(tc);
        else if (error.empty() && r != NULLOBJECT) rexxToVal(side, r, result);   // may run Rexx code
        // The Python objects in the result are borrowed from their .PyObject
        // proxies, which only this thread context's local references keep
        // alive: once it detaches, a collection can run their uninits and
        // another thread's drainGraveyard can free the objects before we take
        // the GIL back.  So take our own references now.  Waiting for the GIL
        // here is safe: between API calls this thread holds no kernel lock.
        if (holdsBorrowed(result))
        {
            Gil gil;
            ownBorrowed(result);
        }
        tc->DetachThread();
    }
    PyEval_RestoreThread(saved);

    // with the GIL: the result
    if (!error.empty())
    {
        dropVal(result);
        PyErr_SetString(rexxErrorType, error.c_str());
        return NULL;
    }
    PyObject *py = valToPy(result);
    dropVal(result);
    return py;
}

// send message to target with args (a tuple); first, if given, goes before
// args (the value of "[]=")
static PyObject *rexxSend(Inst *inst, RexxObjectPtr target, const std::string &message,
                          PyObject *args, PyObject *first = NULL)
{
    return intoRexx(inst, args, 0, first,
        [&](RexxThreadContext *tc, RexxSide &, RexxArrayObject rargs, std::string &) {
            return tc->SendMessage(target, message.c_str(), rargs);
        });
}

static PyObject *rexxProxyCall(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (kwargs != NULL && PyDict_Size(kwargs) > 0)
    {
        PyErr_SetString(PyExc_TypeError, "Rexx methods take no keyword arguments");
        return NULL;
    }
    RexxProxy *p = (RexxProxy *)self;
    return rexxSend(p->inst, p->target, "CALL", args);
}

static bool isSendName(const std::string &s);   // below: sendMessage...

static PyObject *rexxProxyGetAttr(PyObject *self, PyObject *name)
{
    // Python's own protocol names are not Rexx messages (numpy and friends
    // probe __array__, __len__... on any object)
    std::string s = pyString(name);
    if (s.size() >= 2 && s[0] == '_' && s[1] == '_') return PyObject_GenericGetAttr(self, name);
    if (isSendName(s)) return PyObject_GenericGetAttr(self, name);   // sendMessage...: ours
    PyTypeObject *type = (PyTypeObject *)rexxBoundType;
    allocfunc alloc = (allocfunc)PyType_GetSlot(type, Py_tp_alloc);
    RexxBound *b = (RexxBound *)alloc(type, 0);
    if (b == NULL) return NULL;
    Py_INCREF(self);
    b->proxy = self;
    b->message = strdup(upper(s).c_str());
    return (PyObject *)b;
}

static PyObject *rexxProxyGetItem(PyObject *self, PyObject *key)
{
    RexxProxy *p = (RexxProxy *)self;
    PyObject *args = PyTuple_Check(key) ? (Py_INCREF(key), key) : PyTuple_Pack(1, key);
    PyObject *r = rexxSend(p->inst, p->target, "[]", args);
    Py_DECREF(args);
    return r;
}

static int rexxProxySetItem(PyObject *self, PyObject *key, PyObject *value)
{
    RexxProxy *p = (RexxProxy *)self;
    if (value == NULL)
    {
        PyObject *args = PyTuple_Pack(1, key);
        PyObject *r = rexxSend(p->inst, p->target, "REMOVE", args);
        Py_DECREF(args);
        Py_XDECREF(r);
        return r == NULL ? -1 : 0;
    }
    PyObject *keys = PyTuple_Check(key) ? (Py_INCREF(key), key) : PyTuple_Pack(1, key);
    PyObject *r = rexxSend(p->inst, p->target, "[]=", keys, value);
    Py_DECREF(keys);
    Py_XDECREF(r);
    return r == NULL ? -1 : 0;
}

static PyObject *rexxProxyStr(PyObject *self)
{
    RexxProxy *p = (RexxProxy *)self;
    PyObject *r = rexxSend(p->inst, p->target, "STRING", NULL);
    if (r != NULL && !PyUnicode_Check(r))
    {
        PyObject *s = PyObject_Str(r);
        Py_DECREF(r);
        r = s;
    }
    return r;
}

static PyObject *rexxProxyRepr(PyObject *self)
{
    PyObject *s = rexxProxyStr(self);
    if (s == NULL) return NULL;
    PyObject *r = PyUnicode_FromFormat("<Rexx %U>", s);
    Py_DECREF(s);
    return r;
}

static PyObject *rexxProxyIter(PyObject *self)
{
    RexxProxy *p = (RexxProxy *)self;
    PyObject *items = rexxSend(p->inst, p->target, "MAKEARRAY", NULL);
    if (items == NULL) return NULL;
    PyObject *it = PyObject_GetIter(items);
    Py_DECREF(items);
    return it;
}

static Py_ssize_t rexxProxyLen(PyObject *self)
{
    RexxProxy *p = (RexxProxy *)self;
    PyObject *n = rexxSend(p->inst, p->target, "ITEMS", NULL);
    if (n == NULL) return -1;
    Py_ssize_t r = PyLong_Check(n) ? PyLong_AsSsize_t(n) : -1;
    Py_DECREF(n);
    if (r < 0 && !PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "ITEMS did not return a whole number");
    return r;
}

static PyObject *rexxBoundCall(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (kwargs != NULL && PyDict_Size(kwargs) > 0)
    {
        PyErr_SetString(PyExc_TypeError, "Rexx methods take no keyword arguments");
        return NULL;
    }
    RexxBound *b = (RexxBound *)self;
    RexxProxy *p = (RexxProxy *)b->proxy;
    return rexxSend(p->inst, p->target, b->message, args);
}

static void rexxBoundDealloc(PyObject *self)
{
    RexxBound *b = (RexxBound *)self;
    Py_XDECREF(b->proxy);
    free(b->message);
    PyTypeObject *type = Py_TYPE(self);
    freefunc tpFree = (freefunc)PyType_GetSlot(type, Py_tp_free);
    tpFree(self);
    Py_DECREF(type);
}

/*
 * Messages by name, as BSF4ooRexx's RexxProxy (Java) sends them:
 *
 *   r.sendMessage(name[, args])               args: a list or tuple
 *   r.sendMessageScope(name, scope[, args])   the search starts at scope
 *   r.sendMessage0(name) ... r.sendMessage5(name, a1, ..., a5)
 *
 * The name is any string, uppercased as r.name is: one held in a variable,
 * or one that is no Python name ("+", "[]=", "<>").  scope is a Rexx class
 * (an oorexx.RexxObject), as in Rexx's r~name:scope.  These eight names are
 * Python methods, so r.sendMessage is not a message SENDMESSAGE (send that
 * one with r.sendMessage("sendMessage", ...)).
 */
static const char *const sendNames[] =
{
    "sendMessage", "sendMessageScope", "sendMessage0", "sendMessage1",
    "sendMessage2", "sendMessage3", "sendMessage4", "sendMessage5", NULL
};

static bool isSendName(const std::string &s)
{
    for (int i = 0; sendNames[i] != NULL; i++) if (s == sendNames[i]) return true;
    return false;
}

// the message name, uppercased; false (and a TypeError) if not a str
static bool messageName(PyObject *o, const char *method, std::string &name)
{
    if (!PyUnicode_Check(o))
    {
        PyErr_Format(PyExc_TypeError, "%s: the message name must be a str", method);
        return false;
    }
    name = upper(pyString(o));
    return !PyErr_Occurred();
}

// args as a tuple (a new reference): None is no arguments; else a list or tuple
static PyObject *messageArgs(PyObject *o, const char *method)
{
    if (o == NULL || o == Py_None) return PyTuple_New(0);
    if (PyTuple_Check(o) || PyList_Check(o)) return PySequence_Tuple(o);
    PyErr_Format(PyExc_TypeError, "%s: the arguments must be a list or a tuple", method);
    return NULL;
}

static PyObject *rexxProxySendMessage(PyObject *self, PyObject *args)
{
    PyObject *nameObj, *argsObj = NULL;
    if (!PyArg_ParseTuple(args, "O|O:sendMessage", &nameObj, &argsObj)) return NULL;
    std::string name;
    if (!messageName(nameObj, "sendMessage", name)) return NULL;
    PyObject *t = messageArgs(argsObj, "sendMessage");
    if (t == NULL) return NULL;
    RexxProxy *p = (RexxProxy *)self;
    PyObject *r = rexxSend(p->inst, p->target, name, t);
    Py_DECREF(t);
    return r;
}

static PyObject *rexxProxySendMessageScope(PyObject *self, PyObject *args)
{
    PyObject *nameObj, *scopeObj, *argsObj = NULL;
    if (!PyArg_ParseTuple(args, "OO|O:sendMessageScope", &nameObj, &scopeObj, &argsObj)) return NULL;
    std::string name;
    if (!messageName(nameObj, "sendMessageScope", name)) return NULL;
    Inst *scopeInst;
    RexxObjectPtr scope;
    if (!rexxProxyTarget(scopeObj, &scopeInst, &scope))
    {
        PyErr_SetString(PyExc_TypeError, "sendMessageScope: the scope must be a Rexx class (an oorexx.RexxObject)");
        return NULL;
    }
    if (isClosed(scopeInst))
    {
        PyErr_SetString(rexxErrorType, "sendMessageScope: the scope's Rexx interpreter instance has been closed");
        return NULL;
    }
    PyObject *t = messageArgs(argsObj, "sendMessageScope");
    if (t == NULL) return NULL;
    RexxProxy *p = (RexxProxy *)self;
    RexxObjectPtr target = p->target;
    PyObject *r = intoRexx(p->inst, t, 0, NULL,
        [&](RexxThreadContext *tc, RexxSide &, RexxArrayObject rargs, std::string &error) -> RexxObjectPtr {
            if (!tc->IsOfType(scope, "CLASS"))
            {
                error = "sendMessageScope: the scope must be a Rexx class";
                return NULLOBJECT;
            }
            return tc->SendMessageScoped(target, name.c_str(), (RexxClassObject)scope, rargs);
        });
    Py_DECREF(t);
    return r;
}

// sendMessage0 ... sendMessage5: the name and exactly N arguments
template <int N>
static PyObject *rexxProxySendMessageN(PyObject *self, PyObject *args)
{
    char method[16];
    snprintf(method, sizeof method, "sendMessage%d", N);
    if (PyTuple_Size(args) != N + 1)
    {
        PyErr_Format(PyExc_TypeError, "%s takes %d arguments, the message name and %d more (%zd given)",
                     method, N + 1, N, PyTuple_Size(args));
        return NULL;
    }
    std::string name;
    if (!messageName(PyTuple_GetItem(args, 0), method, name)) return NULL;
    RexxProxy *p = (RexxProxy *)self;
    RexxObjectPtr target = p->target;
    return intoRexx(p->inst, args, 1, NULL,
        [&](RexxThreadContext *tc, RexxSide &, RexxArrayObject rargs, std::string &) {
            return tc->SendMessage(target, name.c_str(), rargs);
        });
}

static PyMethodDef rexxProxyMethods[] =
{
    {"sendMessage", rexxProxySendMessage, METH_VARARGS,
     "sendMessage(name[, args]): send the message name (uppercased), with args (a list or tuple)."},
    {"sendMessageScope", rexxProxySendMessageScope, METH_VARARGS,
     "sendMessageScope(name, scope[, args]): the same, the method search starting at scope (a Rexx class)."},
    {"sendMessage0", rexxProxySendMessageN<0>, METH_VARARGS, "sendMessage0(name)"},
    {"sendMessage1", rexxProxySendMessageN<1>, METH_VARARGS, "sendMessage1(name, a1)"},
    {"sendMessage2", rexxProxySendMessageN<2>, METH_VARARGS, "sendMessage2(name, a1, a2)"},
    {"sendMessage3", rexxProxySendMessageN<3>, METH_VARARGS, "sendMessage3(name, a1, a2, a3)"},
    {"sendMessage4", rexxProxySendMessageN<4>, METH_VARARGS, "sendMessage4(name, a1, a2, a3, a4)"},
    {"sendMessage5", rexxProxySendMessageN<5>, METH_VARARGS, "sendMessage5(name, a1, a2, a3, a4, a5)"},
    {NULL, NULL, 0, NULL}
};

static PyType_Slot rexxProxySlots[] =
{
    {Py_tp_methods, (void *)rexxProxyMethods},
    {Py_tp_dealloc, (void *)rexxProxyDealloc},
    {Py_tp_call, (void *)rexxProxyCall},
    {Py_tp_getattro, (void *)rexxProxyGetAttr},
    {Py_mp_subscript, (void *)rexxProxyGetItem},
    {Py_mp_ass_subscript, (void *)rexxProxySetItem},
    {Py_tp_str, (void *)rexxProxyStr},
    {Py_tp_repr, (void *)rexxProxyRepr},
    {Py_tp_iter, (void *)rexxProxyIter},
    {Py_mp_length, (void *)rexxProxyLen},
    {Py_tp_doc, (void *)"A Rexx object (by reference): r(args) sends CALL, r.name(args) sends NAME."},
    {0, NULL}
};

static PyType_Spec rexxProxySpec =
{
    "oorexx.RexxObject", sizeof(RexxProxy), 0, Py_TPFLAGS_DEFAULT, rexxProxySlots
};

static PyType_Slot rexxBoundSlots[] =
{
    {Py_tp_dealloc, (void *)rexxBoundDealloc},
    {Py_tp_call, (void *)rexxBoundCall},
    {0, NULL}
};

static PyType_Spec rexxBoundSpec =
{
    "oorexx.RexxMessage", sizeof(RexxBound), 0, Py_TPFLAGS_DEFAULT, rexxBoundSlots
};

extern PyType_Spec interpreterSpec, ctxSpec;
static PyObject *interpreterType = NULL, *commandContextType = NULL;

// the types, made once (with the GIL)
static bool makeTypes()
{
    if (rexxProxyType != NULL) return true;
    rexxProxyType = PyType_FromSpec(&rexxProxySpec);
    rexxBoundType = PyType_FromSpec(&rexxBoundSpec);
    rexxErrorType = PyErr_NewException("oorexx.RexxError", PyExc_RuntimeError, NULL);
    interpreterType = PyType_FromSpec(&interpreterSpec);
    commandContextType = PyType_FromSpec(&ctxSpec);
    return rexxProxyType != NULL && rexxBoundType != NULL && rexxErrorType != NULL &&
           interpreterType != NULL && commandContextType != NULL;
}

// the module's contents (also when Rexx started Python: then the module is
// put in sys.modules, so Python code can "import oorexx")
static bool fillModule(PyObject *m)
{
    if (!makeTypes()) return false;
    PyObject_SetAttrString(m, "RexxObject", rexxProxyType);
    PyObject_SetAttrString(m, "RexxError", rexxErrorType);
    PyObject_SetAttrString(m, "Interpreter", interpreterType);
    PyObject_SetAttrString(m, "CommandContext", commandContextType);
    return !PyErr_Occurred();
}

static void registerModule()
{
    PyObject *m = PyModule_New("oorexx");
    PyObject *modules = PyImport_GetModuleDict();          // borrowed
    if (m != NULL && fillModule(m)) PyDict_SetItemString(modules, "oorexx", m);
    Py_XDECREF(m);
    PyErr_Clear();
}

/* ------------------------------------------------------------------------ */
/* oorexx.Interpreter: Python creates and uses Rexx interpreter instances     */
/* ------------------------------------------------------------------------ */

/*
 *   r = oorexx.Interpreter(rexx_path=[...], extensions=[".rex", ".cls"],
 *                          initial_address="SYSTEM", load_libraries=[...],
 *                          command_environments={"NAME": callable},
 *                          stdio=True)     (docs/design.md, *Interpreter options*)
 *   r.run("program.rex", args...)      CALL a program: its result
 *   r.routine("use arg a, b; return a * b")   a callable Rexx Routine
 *   r.eval("expression")               the value of a Rexx expression
 *   r.close()                          (also: with oorexx.Interpreter() as r:)
 *
 * python.cls is loaded into every instance (the bridge registers itself as
 * the "rexxpython" library, so no loader is involved), so that Python objects
 * passed to Rexx are .PyObjects and Rexx code can use .py.
 */

struct Interpreter
{
    PyObject_HEAD
    Inst *inst;
};


// the directory oorexx.abi3.so (oorexx.pyd) lives in (python.cls is next to it)
static std::string bridgeDirectory()
{
#ifdef _WIN32
    HMODULE self;
    char buf[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&bridgeDirectory, &self) ||
        GetModuleFileNameA(self, buf, sizeof buf) == 0) return ".";
    std::string path = buf;
    size_t slash = path.find_last_of("\\/");
#else
    Dl_info info;
    if (dladdr((void *)&bridgeDirectory, &info) == 0 || info.dli_fname == NULL) return ".";
    std::string path = info.dli_fname;
    size_t slash = path.rfind('/');
#endif
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

/*
 * SAY and trace output go to Python's sys.stdout / sys.stderr, pulls
 * from the terminal read sys.stdin (so Rexx is not mute in Jupyter).  Exits
 * run with the kernel lock released (docs/design.md, *Threads and locks*,
 * rule 4): the GIL may be taken.
 */
static void writeStream(const char *stream, const char *data, size_t n)
{
    Gil gil;
    PyObject *sys = PyImport_ImportModule("sys");
    PyObject *s = sys != NULL ? PyObject_GetAttrString(sys, stream) : NULL;
    if (s != NULL && s != Py_None)
    {
        std::string line(data, n);
        line += '\n';
        PyObject *text = pyText(line.data(), line.size());
        if (text != NULL) Py_XDECREF(PyObject_CallMethod(s, "write", "O", text));
        Py_XDECREF(text);
    }
    Py_XDECREF(s);
    Py_XDECREF(sys);
    PyErr_Clear();
}

// RexxAllocateMemory (rexxapi): on Windows, ooRexx's portable builds have no
// import library for rexxapi.dll, so it is looked up there (rexx.dll loads it)
static void *rexxAllocate(size_t n)
{
#ifdef _WIN32
    typedef void *(REXXENTRY *AllocFn)(size_t);
    static AllocFn alloc = NULL;
    if (alloc == NULL)
    {
        HMODULE api = GetModuleHandleA("rexxapi.dll");
        if (api == NULL) api = LoadLibraryA("rexxapi.dll");
        if (api != NULL) alloc = (AllocFn)GetProcAddress(api, "RexxAllocateMemory");
        if (alloc == NULL) return NULL;
    }
    return alloc(n);
#else
    return RexxAllocateMemory(n);
#endif
}

static void readStdin(RXSTRING *out)
{
    std::string line;
    {
        Gil gil;
        PyObject *sys = PyImport_ImportModule("sys");
        PyObject *s = sys != NULL ? PyObject_GetAttrString(sys, "stdin") : NULL;
        PyObject *r = s != NULL && s != Py_None ? PyObject_CallMethod(s, "readline", NULL) : NULL;
        if (r != NULL)
        {
            Val v;
            pyToVal(r, v);
            if (v.kind == Val::STR) line = v.s;
            dropVal(v);
        }
        Py_XDECREF(s);
        Py_XDECREF(sys);
        PyErr_Clear();
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (out->strptr == NULL || out->strlength < line.size())
    {
        out->strptr = (char *)rexxAllocate(line.size() + 1);
        if (out->strptr == NULL) { out->strlength = 0; return; }
    }
    memcpy(out->strptr, line.data(), line.size());
    out->strlength = line.size();
}

static int RexxEntry stdioExit(RexxExitContext *, int exitnum, int subfunction, PEXIT parm)
{
    if (exitnum != RXSIO) return RXEXIT_NOT_HANDLED;
    switch (subfunction)
    {
        case RXSIOSAY:
        {
            RXSIOSAY_PARM *p = (RXSIOSAY_PARM *)parm;
            writeStream("stdout", p->rxsio_string.strptr, p->rxsio_string.strlength);
            return RXEXIT_HANDLED;
        }
        case RXSIOTRC:
        {
            RXSIOTRC_PARM *p = (RXSIOTRC_PARM *)parm;
            writeStream("stderr", p->rxsio_string.strptr, p->rxsio_string.strlength);
            return RXEXIT_HANDLED;
        }
        case RXSIOTRD:
            readStdin(&((RXSIOTRD_PARM *)parm)->rxsiotrd_retc);
            return RXEXIT_HANDLED;
        case RXSIODTR:
            readStdin(&((RXSIODTR_PARM *)parm)->rxsiodtr_retc);
            return RXEXIT_HANDLED;
        default:
            return RXEXIT_NOT_HANDLED;
    }
}

extern "C" ORXPY_EXPORT RexxPackageEntry *orxpy_package(void);

// the strings of a str or a list of str (for the options)
static bool stringList(PyObject *o, std::vector<std::string> &out, const char *what)
{
    if (o == NULL || o == Py_None) return true;
    if (PyUnicode_Check(o)) { out.push_back(pyString(o)); return true; }
    PyObject *it = PyObject_GetIter(o);
    if (it == NULL)
    {
        PyErr_Format(PyExc_TypeError, "%s: a str or a list of str", what);
        return false;
    }
    PyObject *item;
    while ((item = PyIter_Next(it)) != NULL)
    {
        out.push_back(pyString(item));
        Py_DECREF(item);
    }
    Py_DECREF(it);
    return !PyErr_Occurred();
}

static std::string join(const std::vector<std::string> &parts, const char *sep)
{
    std::string out;
    for (const std::string &p : parts) out += (out.empty() ? "" : sep) + p;
    return out;
}

static void addCommandEnvironment(RexxInstance *inst, RexxThreadContext *tc, const std::string &name, PyObject *handler);

static PyObject *interpreterNew(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static const char *keywords[] = {"rexx_path", "extensions", "initial_address", "load_libraries",
                                     "command_environments", "stdio", NULL};
    PyObject *rexxPath = NULL, *extensions = NULL, *initialAddress = NULL, *libraries = NULL;
    PyObject *environments = NULL;
    int stdio = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OOOOOp:Interpreter", (char **)keywords,
                                     &rexxPath, &extensions, &initialAddress, &libraries,
                                     &environments, &stdio))
    {
        return NULL;
    }
    std::vector<std::string> path, exts, libs;
    if (!stringList(rexxPath, path, "rexx_path") || !stringList(extensions, exts, "extensions") ||
        !stringList(libraries, libs, "load_libraries"))
    {
        return NULL;
    }
    if (environments != NULL && environments != Py_None && !PyDict_Check(environments))
    {
        PyErr_SetString(PyExc_TypeError, "command_environments: a dict name -> callable");
        return NULL;
    }
    std::string dir = bridgeDirectory();
    path.push_back(dir);
#ifdef _WIN32
    std::string pathText = join(path, ";");
#else
    std::string pathText = join(path, ":");
#endif
    std::string extText = join(exts, ",");
    std::string address = initialAddress != NULL && initialAddress != Py_None ? pyString(initialAddress) : "";

    static RexxLibraryPackage library[] = {{"rexxpython", orxpy_package()}, {NULL, NULL}};
    static RexxContextExit exits[] = {{stdioExit, RXSIO}, {NULL, RXENDLST}};
    std::vector<RexxOption> options;
    RexxOption o;
    o.optionName = EXTERNAL_CALL_PATH; o.option = pathText.c_str(); options.push_back(o);
    if (!exts.empty()) { o.optionName = EXTERNAL_CALL_EXTENSIONS; o.option = extText.c_str(); options.push_back(o); }
    if (!address.empty()) { o.optionName = INITIAL_ADDRESS_ENVIRONMENT; o.option = address.c_str(); options.push_back(o); }
    for (const std::string &l : libs) { o.optionName = LOAD_REQUIRED_LIBRARY; o.option = l.c_str(); options.push_back(o); }
    o.optionName = REGISTER_LIBRARY; o.option = (POINTER)library; options.push_back(o);
    if (stdio) { o.optionName = DIRECT_EXITS; o.option = (POINTER)exits; options.push_back(o); }
    o.optionName = NULL; options.push_back(o);

    Interpreter *self = (Interpreter *)((allocfunc)PyType_GetSlot(type, Py_tp_alloc))(type, 0);
    if (self == NULL) return NULL;

    std::string error;
    RexxInstance *inst = NULL;
    PyThreadState *saved = PyEval_SaveThread();
    RexxThreadContext *tc;
    if (!RexxCreateInterpreter(&inst, &tc, options.data()))
    {
        error = "cannot create a Rexx interpreter instance";
        inst = NULL;
    }
    else
    {
        std::string cls = dir + "/python.cls";
        RexxPackageObject package = tc->LoadPackage(cls.c_str());
        if (package == NULLOBJECT || tc->CheckCondition())
        {
            error = tc->CheckCondition() ? takeCondition(tc) : "cannot load " + cls;
        }
        else
        {
            InstanceData d;
            d.pyObjectClass = (RexxClassObject)tc->RequestGlobalReference(tc->FindPackageClass(package, "PYOBJECT"));
            d.kwClass = (RexxClassObject)tc->RequestGlobalReference(tc->FindPackageClass(package, "PYKWARGS"));
            d.keep = (RexxDirectoryObject)tc->RequestGlobalReference(tc->NewDirectory());
            d.graveyard = graveyardOf(tc, d.pyObjectClass);
            std::lock_guard<std::mutex> guard(instancesLock);
            instances[inst] = d;
        }
        // the creating thread stays attached (its root activity): detaching
        // it makes Terminate wait forever (ooRexx bug #2107, still there in
        // r13268; notes/oorexx-bugs.md)
    }
    PyEval_RestoreThread(saved);
    self->inst = inst != NULL ? newHandle(inst) : NULL;
    if (!error.empty())
    {
        PyErr_SetString(rexxErrorType, error.c_str());
        Py_DECREF(self);
        return NULL;
    }
    if (environments != NULL && environments != Py_None)
    {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(environments, &pos, &key, &value))
        {
            addCommandEnvironment(self->inst->rexx, NULL, pyString(key), value);
        }
    }
    return (PyObject *)self;
}

static bool liveInterpreter(Interpreter *self)
{
    if (self->inst == NULL || isClosed(self->inst))
    {
        PyErr_SetString(rexxErrorType, "the Rexx interpreter instance has been closed");
        return false;
    }
    return true;
}

// r.run(program, args...)
static PyObject *interpreterRun(PyObject *self, PyObject *args)
{
    Interpreter *r = (Interpreter *)self;
    if (!liveInterpreter(r)) return NULL;
    if (PyTuple_Size(args) < 1 || !PyUnicode_Check(PyTuple_GetItem(args, 0)))
    {
        PyErr_SetString(PyExc_TypeError, "run(program, args...): the program name is a str");
        return NULL;
    }
    std::string program = pyString(PyTuple_GetItem(args, 0));
    return intoRexx(r->inst, args, 1, NULL,
        [&](RexxThreadContext *tc, RexxSide &, RexxArrayObject rargs, std::string &) {
            return tc->CallProgram(program.c_str(), rargs);
        });
}

// r.routine(source, name="ROUTINE"): a Routine (callable from Python)
static PyObject *interpreterRoutine(PyObject *self, PyObject *args, PyObject *kwargs)
{
    Interpreter *r = (Interpreter *)self;
    if (!liveInterpreter(r)) return NULL;
    static const char *keywords[] = {"source", "name", NULL};
    PyObject *sourceObj;
    const char *name = "ROUTINE";
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "U|s:routine", (char **)keywords, &sourceObj, &name)) return NULL;
    std::string source = pyString(sourceObj);
    std::string routineName = name;
    return intoRexx(r->inst, NULL, 0, NULL,
        [&](RexxThreadContext *tc, RexxSide &, RexxArrayObject, std::string &) {
            return (RexxObjectPtr)tc->NewRoutine(routineName.c_str(), source.data(), source.size());
        });
}

// r.eval(expression): its value
static PyObject *interpreterEval(PyObject *self, PyObject *args)
{
    Interpreter *r = (Interpreter *)self;
    if (!liveInterpreter(r)) return NULL;
    PyObject *exprObj;
    if (!PyArg_ParseTuple(args, "U:eval", &exprObj)) return NULL;
    std::string source = "return " + pyString(exprObj);
    return intoRexx(r->inst, NULL, 0, NULL,
        [&](RexxThreadContext *tc, RexxSide &, RexxArrayObject rargs, std::string &) -> RexxObjectPtr {
            RexxRoutineObject routine = tc->NewRoutine("EVAL", source.data(), source.size());
            if (routine == NULLOBJECT) return NULLOBJECT;
            return tc->CallRoutine(routine, rargs);
        });
}

// closing: from now on the instance's proxies are unusable and the Rexx
// objects they kept are no longer released one by one
static void forgetInstance(Inst *h)
{
    h->closed = true;
    std::lock_guard<std::mutex> guard(releasesLock);
    for (size_t i = 0; i < pendingReleases.size(); )
    {
        if (pendingReleases[i].first == h)
        {
            pendingReleases[i] = pendingReleases.back();
            pendingReleases.pop_back();
        }
        else i++;
    }
}

// closed: drop what the bridge knew about the instance, unless a new instance
// already reuses its address
static void eraseInstance(Inst *h)
{
    std::lock_guard<std::mutex> guard(instancesLock);
    std::lock_guard<std::mutex> guard2(handlesLock);
    auto it = handles.find(h->rexx);
    if (it == handles.end() || it->second != h) return;
    handles.erase(it);
    instances.erase(h->rexx);
}

// r.close(): terminate the instance (its Rexx objects in Python become unusable)
static PyObject *interpreterClose(PyObject *self, PyObject *)
{
    Interpreter *r = (Interpreter *)self;
    if (r->inst != NULL && !isClosed(r->inst))
    {
        Inst *h = r->inst;
        RexxDirectoryObject keep = NULLOBJECT;
        {
            std::lock_guard<std::mutex> guard(instancesLock);
            auto it = instances.find(h->rexx);
            if (it != instances.end()) keep = it->second.keep;
        }
        forgetInstance(h);
        Py_BEGIN_ALLOW_THREADS
        // the Rexx objects Python still holds through this instance's proxies
        // are let go (the proxies are unusable from now on); those another
        // instance still reaches survive, since all instances share one object
        // memory (docs/design.md, *Lifetime*)
        RexxThreadContext *tc;
        if (keep != NULLOBJECT && h->rexx->AttachThread(&tc))
        {
            tc->SendMessage0(keep, "EMPTY");
            tc->DetachThread();
        }
        h->rexx->Terminate();
        Py_END_ALLOW_THREADS
        eraseInstance(h);
    }
    Py_RETURN_NONE;
}

static PyObject *interpreterEnter(PyObject *self, PyObject *)
{
    Py_INCREF(self);
    return self;
}

static PyObject *interpreterExit(PyObject *self, PyObject *)
{
    return interpreterClose(self, NULL);
}

static PyObject *interpreterAddEnvironment(PyObject *self, PyObject *args);

static void interpreterDealloc(PyObject *self)
{
    // an instance not closed explicitly stays alive (its objects may still be
    // in use from Python, and terminating needs the GIL released)
    PyTypeObject *type = Py_TYPE(self);
    freefunc tpFree = (freefunc)PyType_GetSlot(type, Py_tp_free);
    tpFree(self);
    Py_DECREF(type);
}

static PyMethodDef interpreterMethods[] =
{
    {"run", (PyCFunction)interpreterRun, METH_VARARGS, "run(program, *args): CALL a Rexx program, return its result"},
    {"routine", (PyCFunction)(void (*)(void))interpreterRoutine, METH_VARARGS | METH_KEYWORDS,
     "routine(source, name='ROUTINE'): a Rexx Routine made from source, callable"},
    {"eval", (PyCFunction)interpreterEval, METH_VARARGS, "eval(expression): the value of a Rexx expression"},
    {"add_command_environment", (PyCFunction)interpreterAddEnvironment, METH_VARARGS,
     "add_command_environment(name, handler): ADDRESS name runs handler(command, context)"},
    {"close", (PyCFunction)interpreterClose, METH_NOARGS, "close(): terminate the interpreter instance"},
    {"__enter__", (PyCFunction)interpreterEnter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)interpreterExit, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyType_Slot interpreterSlots[] =
{
    {Py_tp_new, (void *)interpreterNew},
    {Py_tp_dealloc, (void *)interpreterDealloc},
    {Py_tp_methods, (void *)interpreterMethods},
    {Py_tp_doc, (void *)"A Rexx interpreter instance."},
    {0, NULL}
};

PyType_Spec interpreterSpec =
{
    "oorexx.Interpreter", sizeof(Interpreter), 0, Py_TPFLAGS_DEFAULT, interpreterSlots
};

/* ------------------------------------------------------------------------ */
/* ADDRESS environments written in Python (design §5)                         */
/* ------------------------------------------------------------------------ */

/*
 *   def calc(command, ctx):
 *       for line in ctx.lines():          # ADDRESS ... WITH INPUT
 *           ctx.output(str(eval(line)))   # ... WITH OUTPUT (else sys.stdout)
 *       return 0                          # RC
 *   r.add_command_environment("CALC", calc)
 *
 * ctx (valid only while its command runs), as ooRexx/WASM's layer 1
 * (.CommandContext, ref/cmdhandler.cpp):
 *   ctx.lines()  ctx.line_in()            redirected input (all / next, None at end)
 *   ctx.output(text)  ctx.error(text)     redirected output / error
 *   ctx.is_redirected("input"|"output"|"error"|"same")
 *   ctx.get_var(name)  ctx.set_var(name, value)  ctx.drop_var(name)
 *                                         the variables of the issuing code
 *   ctx.raise_error(rc)  ctx.raise_failure(rc)   ERROR / FAILURE with RC
 * The handler's result is RC (None -> 0).  A Python exception in the handler
 * is a SYNTAX error (98.900) in the code that issued the command.  An
 * environment is per interpreter instance; adding a name again replaces it.
 */

struct CommandContext
{
    PyObject_HEAD
    RexxExitContext *exit;
    RexxIORedirectorContext *io;
    bool valid;
    std::string *condition;               // "ERROR" / "FAILURE" asked for
    PyObject *conditionRc;
};

static std::mutex environmentsLock;
static std::map<std::pair<RexxInstance *, std::string>, PyObject *> environmentsByName;

static CommandContext *liveContext(PyObject *self)
{
    CommandContext *c = (CommandContext *)self;
    if (!c->valid)
    {
        PyErr_SetString(rexxErrorType, "this command context belongs to a command that has ended");
        return NULL;
    }
    return c;
}

static PyObject *ctxLineIn(PyObject *self, PyObject *)
{
    CommandContext *c = liveContext(self);
    if (c == NULL) return NULL;
    CSTRING data = NULL;
    size_t length = 0;
    bool any;
    std::string line;
    Py_BEGIN_ALLOW_THREADS
    c->io->ReadInput(&data, &length);
    any = data != NULL;
    if (any) line.assign(data, length);
    Py_END_ALLOW_THREADS
    if (!any) Py_RETURN_NONE;
    return pyText(line.data(), line.size());
}

static PyObject *ctxLines(PyObject *self, PyObject *)
{
    CommandContext *c = liveContext(self);
    if (c == NULL) return NULL;
    std::vector<std::string> lines;
    Py_BEGIN_ALLOW_THREADS
    if (c->io->IsInputRedirected())
    {
        CSTRING data;
        size_t length;
        for (;;)
        {
            data = NULL;
            c->io->ReadInput(&data, &length);
            if (data == NULL) break;
            lines.emplace_back(data, length);
        }
    }
    Py_END_ALLOW_THREADS
    PyObject *list = PyList_New(0);
    for (const std::string &l : lines)
    {
        PyObject *item = pyText(l.data(), l.size());
        PyList_Append(list, item);
        Py_XDECREF(item);
    }
    return list;
}

static PyObject *ctxWrite(PyObject *self, PyObject *args, bool error)
{
    CommandContext *c = liveContext(self);
    if (c == NULL) return NULL;
    PyObject *textObj;
    if (!PyArg_ParseTuple(args, "O", &textObj)) return NULL;
    Val v;
    Py_INCREF(textObj);
    pyToVal(textObj, v);
    std::string text = v.kind == Val::STR ? v.s : pyString(textObj);
    dropVal(v);
    bool redirected;
    Py_BEGIN_ALLOW_THREADS
    redirected = error ? c->io->IsErrorRedirected() : c->io->IsOutputRedirected();
    if (redirected)
    {
        if (error) c->io->WriteError(text.data(), text.size());
        else c->io->WriteOutput(text.data(), text.size());
    }
    Py_END_ALLOW_THREADS
    if (!redirected)
    {
        // like a real command's console output: Python's own streams
        PyObject *sys = PyImport_ImportModule("sys");
        PyObject *s = sys != NULL ? PyObject_GetAttrString(sys, error ? "stderr" : "stdout") : NULL;
        std::string line = text + "\n";
        PyObject *t = pyText(line.data(), line.size());
        if (s != NULL && t != NULL) Py_XDECREF(PyObject_CallMethod(s, "write", "O", t));
        Py_XDECREF(t);
        Py_XDECREF(s);
        Py_XDECREF(sys);
        if (PyErr_Occurred()) return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *ctxOutput(PyObject *self, PyObject *args) { return ctxWrite(self, args, false); }
static PyObject *ctxError(PyObject *self, PyObject *args) { return ctxWrite(self, args, true); }

static PyObject *ctxIsRedirected(PyObject *self, PyObject *args)
{
    CommandContext *c = liveContext(self);
    if (c == NULL) return NULL;
    const char *which;
    if (!PyArg_ParseTuple(args, "s", &which)) return NULL;
    char w = (char)tolower((unsigned char)which[0]);
    bool r = false;
    Py_BEGIN_ALLOW_THREADS
    switch (w)
    {
        case 'i': r = c->io->IsInputRedirected(); break;
        case 'o': r = c->io->IsOutputRedirected(); break;
        case 'e': r = c->io->IsErrorRedirected(); break;
        case 's': r = c->io->AreOutputAndErrorSameTarget(); break;
    }
    Py_END_ALLOW_THREADS
    return PyBool_FromLong(r);
}

static PyObject *ctxGetVar(PyObject *self, PyObject *args)
{
    CommandContext *c = liveContext(self);
    if (c == NULL) return NULL;
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return NULL;
    std::string n = upper(name);
    Val v;
    Py_BEGIN_ALLOW_THREADS
    RexxThreadContext *tc = c->exit->threadContext;
    RexxObjectPtr o = c->exit->GetContextVariable(n.c_str());
    RexxSide side;
    if (o != NULLOBJECT && sideFor(tc, side)) rexxToVal(side, o, v);
    Py_END_ALLOW_THREADS
    PyObject *r = valToPy(v);
    dropVal(v);
    return r;
}

static PyObject *ctxSetVar(PyObject *self, PyObject *args)
{
    CommandContext *c = liveContext(self);
    if (c == NULL) return NULL;
    const char *name;
    PyObject *value;
    if (!PyArg_ParseTuple(args, "sO", &name, &value)) return NULL;
    std::string n = upper(name);
    Val v;
    Py_INCREF(value);
    pyToVal(value, v);
    Py_BEGIN_ALLOW_THREADS
    RexxThreadContext *tc = c->exit->threadContext;
    RexxSide side;
    if (!sideFor(tc, side)) { side.tc = tc; side.inst = tc->instance; side.pyObjectClass = NULLOBJECT; side.kwClass = NULLOBJECT; }
    RexxObjectPtr o = valToRexx(side, v);
    c->exit->SetContextVariable(n.c_str(), o);
    Py_END_ALLOW_THREADS
    dropVal(v);
    Py_RETURN_NONE;
}

static PyObject *ctxDropVar(PyObject *self, PyObject *args)
{
    CommandContext *c = liveContext(self);
    if (c == NULL) return NULL;
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return NULL;
    std::string n = upper(name);
    Py_BEGIN_ALLOW_THREADS
    c->exit->DropContextVariable(n.c_str());
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyObject *ctxRaise(PyObject *self, PyObject *args, const char *condition)
{
    CommandContext *c = liveContext(self);
    if (c == NULL) return NULL;
    PyObject *rc = NULL;
    if (!PyArg_ParseTuple(args, "|O", &rc)) return NULL;
    *c->condition = condition;
    Py_XDECREF(c->conditionRc);
    c->conditionRc = rc;
    Py_XINCREF(rc);
    if (rc == NULL) Py_RETURN_NONE;
    Py_INCREF(rc);
    return rc;
}

static PyObject *ctxRaiseError(PyObject *self, PyObject *args) { return ctxRaise(self, args, "ERROR"); }
static PyObject *ctxRaiseFailure(PyObject *self, PyObject *args) { return ctxRaise(self, args, "FAILURE"); }

static void ctxDealloc(PyObject *self)
{
    CommandContext *c = (CommandContext *)self;
    Py_XDECREF(c->conditionRc);
    PyTypeObject *type = Py_TYPE(self);
    freefunc tpFree = (freefunc)PyType_GetSlot(type, Py_tp_free);
    tpFree(self);
    Py_DECREF(type);
}

static PyMethodDef ctxMethods[] =
{
    {"line_in", (PyCFunction)ctxLineIn, METH_NOARGS, "the next redirected input line, None at the end"},
    {"lines", (PyCFunction)ctxLines, METH_NOARGS, "all the redirected input lines"},
    {"output", (PyCFunction)ctxOutput, METH_VARARGS, "write a line to the command's output"},
    {"error", (PyCFunction)ctxError, METH_VARARGS, "write a line to the command's error"},
    {"is_redirected", (PyCFunction)ctxIsRedirected, METH_VARARGS, "is_redirected('input'|'output'|'error'|'same')"},
    {"get_var", (PyCFunction)ctxGetVar, METH_VARARGS, "a variable of the code that issued the command (None if unset)"},
    {"set_var", (PyCFunction)ctxSetVar, METH_VARARGS, "set a variable of the code that issued the command"},
    {"drop_var", (PyCFunction)ctxDropVar, METH_VARARGS, "drop a variable of the code that issued the command"},
    {"raise_error", (PyCFunction)ctxRaiseError, METH_VARARGS, "raise ERROR after the command (returns rc)"},
    {"raise_failure", (PyCFunction)ctxRaiseFailure, METH_VARARGS, "raise FAILURE after the command (returns rc)"},
    {NULL, NULL, 0, NULL}
};

static PyType_Slot ctxSlots[] =
{
    {Py_tp_dealloc, (void *)ctxDealloc},
    {Py_tp_methods, (void *)ctxMethods},
    {Py_tp_doc, (void *)"The context of an ADDRESS command handled by Python."},
    {0, NULL}
};

PyType_Spec ctxSpec =
{
    "oorexx.CommandContext", sizeof(CommandContext), 0, Py_TPFLAGS_DEFAULT, ctxSlots
};

// the one native handler of every environment written in Python
static RexxObjectPtr RexxEntry pythonCommandHandler(RexxExitContext *c, RexxStringObject address,
    RexxStringObject command, RexxIORedirectorContext *io)
{
    std::string name = upper(std::string(c->StringData(address), c->StringLength(address)));
    std::string text(c->StringData(command), c->StringLength(command));
    RexxInstance *inst = c->threadContext->instance;
    PyObject *handler = NULL;
    {
        // our own reference, taken with the GIL: addCommandEnvironment may
        // replace (and drop) this handler meanwhile on another thread
        Gil gil;
        std::lock_guard<std::mutex> guard(environmentsLock);
        auto it = environmentsByName.find(std::make_pair(inst, name));
        if (it != environmentsByName.end()) { handler = it->second; Py_INCREF(handler); }
    }
    if (handler == NULL)
    {
        RexxObjectPtr rc = c->WholeNumber(RXSUBCOM_NOTREG);
        c->RaiseCondition("FAILURE", command, NULLOBJECT, rc);
        return rc;
    }

    std::string condition;
    Val rcVal;
    PyError err;
    {
        Gil gil;
        PyTypeObject *type = (PyTypeObject *)commandContextType;
        CommandContext *ctx = (CommandContext *)((allocfunc)PyType_GetSlot(type, Py_tp_alloc))(type, 0);
        if (ctx == NULL) { captureError(err); }
        else
        {
            ctx->exit = c;
            ctx->io = io;
            ctx->valid = true;
            ctx->condition = &condition;
            ctx->conditionRc = NULL;
            PyObject *cmd = pyText(text.data(), text.size());
            PyObject *r = cmd != NULL ? PyObject_CallFunctionObjArgs(handler, cmd, (PyObject *)ctx, NULL) : NULL;
            Py_XDECREF(cmd);
            ctx->valid = false;
            if (r == NULL) captureError(err);
            else
            {
                if (r == Py_None && ctx->conditionRc != NULL)
                {
                    Py_DECREF(r);
                    r = ctx->conditionRc;
                    Py_INCREF(r);
                }
                pyToVal(r, rcVal);
            }
            ctx->condition = NULL;
            Py_DECREF((PyObject *)ctx);
        }
        Py_DECREF(handler);
    }
    if (err.set)
    {
        std::string message = "Python error: " + err.message;
        if (!message.empty() && message.back() == '.') message.pop_back();
        RexxArrayObject extra = c->NewArray(2);
        c->ArrayPut(extra, c->String(message.c_str()), 1);
        c->ArrayPut(extra, c->String(err.traceback.c_str()), 2);
        c->RaiseException(Rexx_Error_Execution_user_defined, extra);
        return NULLOBJECT;
    }
    RexxSide side;
    if (!sideFor(c->threadContext, side))
    {
        side.tc = c->threadContext; side.inst = inst; side.pyObjectClass = NULLOBJECT; side.kwClass = NULLOBJECT;
    }
    RexxObjectPtr rc = rcVal.kind == Val::NIL ? c->WholeNumber(0) : valToRexx(side, rcVal);
    if (holdsPython(rcVal)) { Gil gil; dropVal(rcVal); }
    if (!condition.empty()) c->RaiseCondition(condition.c_str(), command, NULLOBJECT, rc);
    return rc;
}

// make name an ADDRESS environment of inst served by handler (tc: a thread
// context of inst, or NULL: this Python thread attaches; called with the GIL)
static void addCommandEnvironment(RexxInstance *inst, RexxThreadContext *tc, const std::string &rawName, PyObject *handler)
{
    std::string name = upper(rawName);
    {
        std::lock_guard<std::mutex> guard(environmentsLock);
        auto key = std::make_pair(inst, name);
        auto it = environmentsByName.find(key);
        Py_INCREF(handler);
        if (it != environmentsByName.end()) { Py_DECREF(it->second); it->second = handler; }
        else environmentsByName[key] = handler;
    }
    Py_BEGIN_ALLOW_THREADS
    RexxThreadContext *t = tc;
    bool attached = false;
    if (t == NULL) attached = inst->AttachThread(&t);
    if (t != NULL) t->AddCommandEnvironment(name.c_str(), (REXXPFN)pythonCommandHandler, REDIRECTING_COMMAND_ENVIRONMENT);
    if (attached) t->DetachThread();
    Py_END_ALLOW_THREADS
}

// r.add_command_environment(name, handler)
static PyObject *interpreterAddEnvironment(PyObject *self, PyObject *args)
{
    Interpreter *r = (Interpreter *)self;
    if (!liveInterpreter(r)) return NULL;
    PyObject *nameObj, *handler;
    if (!PyArg_ParseTuple(args, "UO:add_command_environment", &nameObj, &handler)) return NULL;
    if (!PyCallable_Check(handler))
    {
        PyErr_SetString(PyExc_TypeError, "the handler must be callable: handler(command, context)");
        return NULL;
    }
    addCommandEnvironment(r->inst->rexx, NULL, pyString(nameObj), handler);
    Py_RETURN_NONE;
}

/* ------------------------------------------------------------------------ */
/* .PyObject                                                                  */
/* ------------------------------------------------------------------------ */

static PyObject *selfObject(void *cself)
{
    return (PyObject *)cself;
}

RexxMethod1(RexxObjectPtr, PyObj_Pointer, CSELF, self)
{
    return context->NewPointer(self);
}

/*
 * A .PyObject's uninit is Rexx code (python.cls) that only queues its pointer
 * on .PyObject~graveyard; the Python reference is dropped here, by the next
 * native method that runs in that instance.  Why not a native uninit: ooRexx's
 * MemoryObject::runUninits() iterates its uninit table while running each
 * uninit, and a native method releases the kernel lock while it runs
 * (NativeActivation::run -> releaseAccess), all the more if it then waits for
 * the GIL.  Other threads then create objects with uninit methods, the table
 * changes under the iterator, and the interpreter crashes (ooRexx bug #2101,
 * fixed in r13256; notes/oorexx-bugs.md).  A short Rexx uninit that sends
 * only built-in messages never releases the kernel lock.
 *
 * Phase 1 (no GIL): pull the pointers; phase 2 (GIL): drop the references.
 */
static void drainGraveyard(RexxThreadContext *tc, RexxObjectPtr graveyard)
{
    if (graveyard == NULLOBJECT) return;
    std::vector<PyObject *> dead;
    for (;;)
    {
        RexxObjectPtr p = tc->SendMessage0(graveyard, "PULL");
        if (p == NULLOBJECT || p == tc->Nil()) break;
        void *v = tc->PointerValue((RexxPointerObject)p);
        if (v != NULL) dead.push_back((PyObject *)v);
    }
    if (dead.empty() || !Py_IsInitialized()) return;
    Gil gil;
    for (PyObject *o : dead) Py_DECREF(o);
}

/*
 * ~unknown: o~name(args) and o~name = value (docs/design.md, *Names*).
 *   - o~name = v: setattr on the resolved name (it must exist: Rexx cannot
 *     tell its case; setattr("exactName", v) creates new ones);
 *   - o~name(args...): the attribute, called with the arguments;
 *   - o~name with no arguments: called if it is a function or method (not a
 *     class or module), else the attribute itself;
 *   - o~new(args) on a callable with no "new" attribute: o(args), so that
 *     classes can be instantiated the Rexx way.
 */
RexxMethod3(RexxObjectPtr, PyObj_Unknown, CSTRING, message, RexxArrayObject, rexxArgs_, CSELF, self)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    std::string name = message;
    bool assign = name.size() > 1 && name.back() == '=';
    if (assign) name.pop_back();
    Args args;
    rexxArgs(side, rexxArgs_, 1, args);

    Val result;
    PyError err;
    {
        Gil gil;
        PyObject *o = selfObject(self);
        std::string exact;
        bool found = resolveName(o, name, &exact, err);
        if (!found && !err.set)
        {
            if (!assign && name == "NEW" && PyCallable_Check(o))
            {
                callInto(o, args, result, err);
            }
            else
            {
                PyObject *typeName = PyObject_GetAttrString((PyObject *)Py_TYPE(o), "__name__");
                std::string tn = pyString(typeName);
                Py_XDECREF(typeName);
                bridgeError(err, "AttributeError: Python object of type " + tn +
                    " has no attribute " + lower(name) +
                    (assign ? " (setattr creates an attribute with its exact name)"
                            : " in any case"));
            }
        }
        else if (found && assign)
        {
            PyObject *value = args.positional.empty() ? NULL : valToPy(args.positional[0]);
            if (value == NULL || PyObject_SetAttrString(o, exact.c_str(), value) < 0) captureError(err);
            Py_XDECREF(value);
        }
        else if (found)
        {
            PyObject *attr = PyObject_GetAttrString(o, exact.c_str());
            if (attr == NULL)
            {
                captureError(err);
            }
            else if (!args.positional.empty() || !args.keywords.empty() ||
                     (PyCallable_Check(attr) && !PyType_Check(attr) && !PyModule_Check(attr)))
            {
                callInto(attr, args, result, err);
                Py_DECREF(attr);
            }
            else
            {
                pyToVal(attr, result);
            }
        }
    }
    return finish(side, result, err);
}

// ~"PY.HAS"(rexxName): does the name resolve (docs/design.md, *Names*) to an attribute?
RexxMethod2(logical_t, PyObj_Has, CSTRING, name, CSELF, self)
{
    Gil gil;
    std::string exact;
    PyError err;
    return resolveName(selfObject(self), name, &exact, err);
}

// ~getattr(exactName)
RexxMethod2(RexxObjectPtr, PyObj_GetAttr, CSTRING, name, CSELF, self)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    Val result;
    PyError err;
    {
        Gil gil;
        PyObject *attr = PyObject_GetAttrString(selfObject(self), name);
        if (attr == NULL) captureError(err); else pyToVal(attr, result);
    }
    return finish(side, result, err);
}

// ~setattr(exactName, value)
RexxMethod3(RexxObjectPtr, PyObj_SetAttr, CSTRING, name, RexxObjectPtr, value, CSELF, self)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    Val result;
    Args holder;
    holder.positional.emplace_back();
    rexxToVal(side, value, holder.positional[0]);
    PyError err;
    {
        Gil gil;
        PyObject *pv = valToPy(holder.positional[0]);
        if (pv == NULL || PyObject_SetAttrString(selfObject(self), name, pv) < 0) captureError(err);
        Py_XDECREF(pv);
    }
    return finish(side, result, err);
}

// ~invoke(exactName | .nil, args...): call the attribute, or the object itself
RexxMethod2(RexxObjectPtr, PyObj_Invoke, ARGLIST, all, CSELF, self)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    RexxObjectPtr target = context->ArraySize(all) >= 1 ? context->ArrayAt(all, 1) : NULLOBJECT;
    bool callSelf = target == NULLOBJECT || target == context->Nil();
    std::string name = callSelf ? "" : context->ObjectToStringValue(target);
    Args args;
    rexxArgs(side, all, 2, args);
    Val result;
    PyError err;
    {
        Gil gil;
        PyObject *f = callSelf ? selfObject(self) : PyObject_GetAttrString(selfObject(self), name.c_str());
        if (f == NULL) captureError(err);
        else
        {
            callInto(f, args, result, err);
            if (!callSelf) Py_DECREF(f);
        }
    }
    return finish(side, result, err);
}

// the key of o[k] / o[k1, k2] (a tuple: numpy a[i, j])
static PyObject *itemKey(const Args &keys)
{
    if (keys.positional.size() == 1) return valToPy(keys.positional[0]);
    PyObject *tuple = PyTuple_New((Py_ssize_t)keys.positional.size());
    for (size_t i = 0; tuple != NULL && i < keys.positional.size(); i++)
    {
        PyObject *item = valToPy(keys.positional[i]);
        if (item == NULL) { Py_CLEAR(tuple); break; }
        PyTuple_SetItem(tuple, (Py_ssize_t)i, item);
    }
    return tuple;
}

// o[keys...]
RexxMethod2(RexxObjectPtr, PyObj_GetItem, ARGLIST, all, CSELF, self)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    Args keys;
    rexxArgs(side, all, 1, keys);
    Val result;
    PyError err;
    {
        Gil gil;
        PyObject *key = itemKey(keys);
        PyObject *r = key != NULL ? PyObject_GetItem(selfObject(self), key) : NULL;
        Py_XDECREF(key);
        if (r == NULL) captureError(err); else pyToVal(r, result);
    }
    return finish(side, result, err);
}

// o[keys...] = value  (Rexx passes the value first)
RexxMethod2(RexxObjectPtr, PyObj_SetItem, ARGLIST, all, CSELF, self)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    Val result;
    Args keys;
    rexxArgs(side, all, 2, keys);
    keys.keywords.emplace_back();         // the value, kept for dropping
    Val &value = keys.keywords.back().second;
    rexxToVal(side, context->ArrayAt(all, 1), value);
    PyError err;
    {
        Gil gil;
        // A Python caller of o[k] = v holds o in its frame during the call,
        // besides the variable that names it; libraries that look at
        // sys.getrefcount(self) to detect chained assignment (pandas) count
        // on that, and with only the proxy's reference they take o for a
        // temporary (pandas 2 warns; pandas 3 then looks for the caller's
        // frame, finds none and fails).  So the call holds a reference too.
        PyObject *o = selfObject(self);
        Py_INCREF(o);
        PyObject *key = itemKey(keys);
        PyObject *pv = key != NULL ? valToPy(value) : NULL;
        if (pv == NULL || PyObject_SetItem(o, key, pv) < 0) captureError(err);
        Py_XDECREF(pv);
        Py_XDECREF(key);
        Py_DECREF(o);
    }
    return finish(side, result, err);
}

// ~string: str(o)
RexxMethod1(RexxObjectPtr, PyObj_String, CSELF, self)
{
    std::string text;
    {
        Gil gil;
        text = pyString(selfObject(self));
    }
    return context->String(text.data(), text.size());
}

// ~makeArray: the items of iter(o), converted (DO OVER)
RexxMethod1(RexxObjectPtr, PyObj_MakeArray, CSELF, self)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    Val result;
    result.kind = Val::LIST;
    PyError err;
    {
        Gil gil;
        PyObject *it = PyObject_GetIter(selfObject(self));
        PyObject *item;
        while (it != NULL && (item = PyIter_Next(it)) != NULL)
        {
            result.items.emplace_back();
            pyToVal(item, result.items.back());
        }
        Py_XDECREF(it);
        if (PyErr_Occurred()) captureError(err);
    }
    return finish(side, result, err);
}

/*
 * Rexx operators on a .PyObject: Python's operators (module operator), so
 * that numpy arrays and Python numbers work in Rexx expressions (a + b,
 * a * 2, -a, a < b).  The left operand must be the Python object (Rexx sends
 * the message to it); "==" and "\==" are identity (Python's "is"), as Rexx
 * collections expect, "=" and "\=" equality (__eq__/__ne__).
 */
static const char *operatorFunction(const std::string &op, bool unary)
{
    static const struct { const char *rexx, *binary, *unary; } table[] =
    {
        {"+", "add", "pos"}, {"-", "sub", "neg"}, {"*", "mul", NULL},
        {"/", "truediv", NULL}, {"%", "floordiv", NULL}, {"//", "mod", NULL},
        {"**", "pow", NULL}, {"<", "lt", NULL}, {">", "gt", NULL},
        {"<=", "le", NULL}, {">=", "ge", NULL}, {"=", "eq", NULL},
        {"\\=", "ne", NULL}, {"<>", "ne", NULL}, {"><", "ne", NULL},
        {"==", "is_", NULL}, {"\\==", "is_not", NULL},
        {"&", "and_", NULL}, {"|", "or_", NULL}, {"&&", "xor", NULL},
        {"\\", NULL, "not_"},
    };
    for (const auto &e : table)
    {
        if (op == e.rexx) return unary ? e.unary : e.binary;
    }
    return NULL;
}

RexxMethod3(RexxObjectPtr, PyObj_Operator, NAME, message, ARGLIST, all, CSELF, self)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    std::string op = message;
    Args args;
    rexxArgs(side, all, 1, args);
    bool unary = args.positional.empty();
    const char *fname = operatorFunction(op, unary);
    Val result;
    PyError err;
    {
        Gil gil;
        if (fname == NULL) bridgeError(err, "TypeError: operator " + op + " needs " +
                                       (unary ? "two operands" : "one operand"));
        else
        {
            PyObject *module = PyImport_ImportModule("operator");
            PyObject *f = module != NULL ? PyObject_GetAttrString(module, fname) : NULL;
            if (f == NULL) captureError(err);
            else
            {
                Val me;
                me.kind = Val::REF;
                me.ref = selfObject(self);
                args.positional.insert(args.positional.begin(), me);
                callInto(f, args, result, err);
                Py_DECREF(f);
            }
            Py_XDECREF(module);
        }
    }
    return finish(side, result, err);
}

// ~hashCode: consistent with "==" (identity), for Rexx collections
RexxMethod1(RexxObjectPtr, PyObj_HashCode, CSELF, self)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%p", self);
    return context->String(buf);
}

/* ------------------------------------------------------------------------ */
/* .py (a .PyObject for builtins, plus the bridge's own helpers)              */
/* ------------------------------------------------------------------------ */

// .py~import(name): the module, always as a proxy
RexxMethod1(RexxObjectPtr, Py_Import, CSTRING, name)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    Val result;
    PyError err;
    {
        Gil gil;
        PyObject *m = PyImport_ImportModule(name);
        if (m == NULL) captureError(err); else pyToVal(m, result, true);
    }
    return finish(side, result, err);
}

// the builtins module (a new reference)
static PyObject *builtinsModule()
{
    return PyImport_ImportModule("builtins");
}

// the object .py stands for: a .Pointer holding a reference to builtins
/*
 * .PyObject~graveyard (python.cls, the class's activate): one Queue for the
 * whole process.  A package loaded while no other instance is alive is loaded
 * afresh, with a new .PyObject class; a graveyard per class would be orphaned
 * when its last instance closes, with the pointers of the uninits that ran
 * while it terminated (and of any of its .PyObjects another instance still
 * reaches, when they die).  A global reference keeps the Queue: those survive
 * the instance that made them.
 */
RexxMethod0(RexxObjectPtr, Py_Graveyard)
{
    static std::mutex lock;
    static RexxObjectPtr graveyard = NULLOBJECT;
    std::lock_guard<std::mutex> guard(lock);
    if (graveyard == NULLOBJECT)
    {
        RexxObjectPtr q = context->SendMessage0(context->FindClass("QUEUE"), "NEW");
        if (q != NULLOBJECT) graveyard = context->RequestGlobalReference(q);
    }
    return graveyard != NULLOBJECT ? graveyard : context->Nil();
}

RexxMethod0(RexxObjectPtr, Py_Builtins)
{
    PyObject *b;
    {
        Gil gil;
        b = builtinsModule();
        PyErr_Clear();
    }
    return context->NewPointer(b);
}

/*
 * .py~str(x), ~int, ~float, ~decimal: Python's str(x), int(x), float(x),
 * decimal.Decimal(x), with the same arguments, but the result is kept as a
 * proxy (not converted back to a Rexx string), so that passing it on keeps its
 * type (docs/design.md, *Values*).  Every other builtin (list, tuple...) is
 * reached through UNKNOWN, unchanged (docs/design.md, *The .py object*).
 */
RexxMethod2(RexxObjectPtr, Py_Make, NAME, message, ARGLIST, all)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    std::string kind = lower(message);
    Args args;
    rexxArgs(side, all, 1, args);
    Val result;
    PyError err;
    {
        Gil gil;
        PyObject *module = kind == "decimal" ? PyImport_ImportModule("decimal") : builtinsModule();
        PyObject *type = module != NULL
            ? PyObject_GetAttrString(module, kind == "decimal" ? "Decimal" : kind.c_str()) : NULL;
        if (type == NULL) captureError(err);
        else
        {
            // the argument goes as text for decimal (all Rexx digits kept)
            if (kind == "decimal" && args.positional.size() == 1 &&
                args.positional[0].kind == Val::STR)
            {
                PyObject *text = pyText(args.positional[0].s.data(), args.positional[0].s.size());
                PyObject *r = text != NULL ? PyObject_CallFunctionObjArgs(type, text, NULL) : NULL;
                Py_XDECREF(text);
                if (r == NULL) captureError(err); else pyToVal(r, result, true);
            }
            else
            {
                callInto(type, args, result, err, true);
            }
            Py_DECREF(type);
        }
        Py_XDECREF(module);
    }
    return finish(side, result, err);
}

/*
 * .py~box(type, value) and py.box (argument order as BSF4ooRexx's box;
 * docs/design.md, *Boxing*): the Rexx value as a Python object of the given
 * type, built from its text, never through the *Values* rule.
 *   type: "str", "int", "float", "bool" (Rexx 1/0), "bytes",
 *         "complex", "decimal", or any Python callable (a .PyObject: a class
 *         or a function), which receives the value as a str.
 * The result is kept as a proxy (passing it on keeps its type).
 */
RexxMethod2(RexxObjectPtr, Py_Box, RexxObjectPtr, type, RexxObjectPtr, value)   // (type, value), as BSF4ooRexx's box
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    std::string text = context->ObjectToStringValue(value);
    std::string kind = "str";
    Args holder;                          // a Python callable given as the type
    if (type != NULLOBJECT)
    {
        if (context->IsInstanceOf(type, side.pyObjectClass))
        {
            kind = "";
            holder.positional.emplace_back();
            rexxToVal(side, type, holder.positional[0]);
        }
        else kind = lower(context->ObjectToStringValue(type));
    }
    Val result;
    PyError err;
    {
        Gil gil;
        PyObject *str = pyText(text.data(), text.size());
        PyObject *r = NULL;
        if (str == NULL) captureError(err);
        else if (kind == "str") { r = str; Py_INCREF(r); }
        else if (kind == "bytes") r = PyBytes_FromStringAndSize(text.data(), (Py_ssize_t)text.size());
        else if (kind == "bool")
        {
            if (text == "1" || text == "0") r = PyBool_FromLong(text == "1");
            else bridgeError(err, "ValueError: box as bool: a Rexx logical value (1 or 0), not " + text);
        }
        else
        {
            PyObject *f = NULL;
            if (kind.empty()) { f = holder.positional[0].ref; Py_XINCREF(f); }
            else if (kind == "decimal" || kind == "fraction")
            {
                PyObject *m = PyImport_ImportModule(kind == "decimal" ? "decimal" : "fractions");
                f = m != NULL ? PyObject_GetAttrString(m, kind == "decimal" ? "Decimal" : "Fraction") : NULL;
                Py_XDECREF(m);
            }
            else if (kind == "int" || kind == "float" || kind == "complex")
            {
                PyObject *b = builtinsModule();
                f = b != NULL ? PyObject_GetAttrString(b, kind.c_str()) : NULL;
                Py_XDECREF(b);
            }
            else bridgeError(err, "ValueError: box: unknown type " + kind +
                             " (str, int, float, bool, bytes, complex, decimal, fraction, or a Python callable)");
            if (f != NULL) r = PyObject_CallFunctionObjArgs(f, str, NULL);
            Py_XDECREF(f);
        }
        Py_XDECREF(str);
        if (r != NULL) pyToVal(r, result, true);
        else if (!err.set) captureError(err);
    }
    return finish(side, result, err);
}

/* ------------------------------------------------------------------------ */
/* The package                                                                */
/* ------------------------------------------------------------------------ */

static void RexxEntry packageLoad(RexxThreadContext *)
{
    startPython();
}

// Python is never finalized (extension modules do not survive it well, and
// other interpreter instances may still use it); its output is flushed.
static void RexxEntry packageUnload(RexxThreadContext *)
{
    if (getenv("ORXPY_DEBUG") != NULL)
    {
        fprintf(stderr, "orxpy: global refs %ld, released %ld, pending %zu; Rexx proxies %ld, freed %ld\n",
                statGlobalRefs.load(), statReleased.load(), pendingReleases.size(),
                statProxies.load(), statProxiesFreed.load());
    }
    if (!Py_IsInitialized()) return;
    Gil gil;
    PyObject *sys = PyImport_ImportModule("sys");
    for (const char *stream : {"stdout", "stderr"})
    {
        PyObject *s = sys != NULL ? PyObject_GetAttrString(sys, stream) : NULL;
        if (s != NULL) Py_XDECREF(PyObject_CallMethod(s, "flush", NULL));
        Py_XDECREF(s);
    }
    Py_XDECREF(sys);
    PyErr_Clear();
}

static RexxMethodEntry bridgeMethods[] =
{
    REXX_METHOD(PyObj_Pointer,   PyObj_Pointer),
    REXX_METHOD(PyObj_Unknown,   PyObj_Unknown),
    REXX_METHOD(PyObj_GetAttr,   PyObj_GetAttr),
    REXX_METHOD(PyObj_Has,       PyObj_Has),
    REXX_METHOD(PyObj_SetAttr,   PyObj_SetAttr),
    REXX_METHOD(PyObj_Invoke,    PyObj_Invoke),
    REXX_METHOD(PyObj_GetItem,   PyObj_GetItem),
    REXX_METHOD(PyObj_SetItem,   PyObj_SetItem),
    REXX_METHOD(PyObj_String,    PyObj_String),
    REXX_METHOD(PyObj_MakeArray, PyObj_MakeArray),
    REXX_METHOD(PyObj_Operator,  PyObj_Operator),
    REXX_METHOD(PyObj_HashCode,  PyObj_HashCode),
    REXX_METHOD(Py_Import,       Py_Import),
    REXX_METHOD(Py_Builtins,     Py_Builtins),
    REXX_METHOD(Py_Graveyard,    Py_Graveyard),
    REXX_METHOD(Py_Make,         Py_Make),
    REXX_METHOD(Py_Box,          Py_Box),
    REXX_LAST_METHOD()
};

static RexxPackageEntry bridgePackage =
{
    STANDARD_PACKAGE_HEADER
    REXX_INTERPRETER_5_0_0,
    "rexxpython",
    "0.1",
    packageLoad,
    packageUnload,
    NULL,
    bridgeMethods
};

extern "C" ORXPY_EXPORT
RexxPackageEntry *orxpy_package(void)
{
    return &bridgePackage;
}

/* ------------------------------------------------------------------------ */
/* The Python module (when Python starts: import oorexx)                      */
/* ------------------------------------------------------------------------ */

static struct PyModuleDef moduleDef =
{
    PyModuleDef_HEAD_INIT, "oorexx",
    "ooRexx from Python: oorexx.Interpreter, oorexx.RexxObject, oorexx.RexxError.",
    -1, NULL, NULL, NULL, NULL, NULL
};

#ifdef _WIN32
/*
 * Windows: rexx.dll is delay-loaded (build.sh), because Python (3.8 and
 * later) loads an extension module's DLLs without searching the PATH, where
 * ooRexx's bin directory is.  Before the first call into it, find it there
 * ourselves: once loaded, the delay-load helper finds it by name.
 */
static bool preloadRexx()
{
    if (GetModuleHandleA("rexx.dll") != NULL) return true;
    const char *home = getenv("OOREXX_HOME");
    std::string dirs = home != NULL ? std::string(home) + "\\bin;" : "";
    const char *path = getenv("PATH");
    if (path != NULL) dirs += path;
    size_t start = 0;
    while (start <= dirs.size())
    {
        size_t end = dirs.find(';', start);
        if (end == std::string::npos) end = dirs.size();
        std::string dir = dirs.substr(start, end - start);
        start = end + 1;
        if (dir.empty()) continue;
        std::string dll = dir + "\\rexx.dll";
        if (GetFileAttributesA(dll.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (LoadLibraryExA(dll.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH) != NULL) return true;
    }
    PyErr_SetString(PyExc_ImportError,
        "oorexx: rexx.dll not found: put ooRexx's bin directory on the PATH (or set OOREXX_HOME)");
    return false;
}
#endif

PyMODINIT_FUNC PyInit_oorexx(void)
{
#ifdef _WIN32
    if (!preloadRexx()) return NULL;
#endif
    PyObject *m = PyModule_Create(&moduleDef);
    if (m == NULL) return NULL;
    if (!fillModule(m)) { Py_DECREF(m); return NULL; }
    return m;
}
