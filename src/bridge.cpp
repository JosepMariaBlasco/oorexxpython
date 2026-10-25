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
#include <signal.h>
#define ORXPY_EXPORT __declspec(dllexport)
static void setEnv(const char *name, const char *value) { _putenv_s(name, value); }
static void unsetEnv(const char *name) { _putenv_s(name, ""); }
#else
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#define ORXPY_EXPORT __attribute__((visibility("default")))
static void setEnv(const char *name, const char *value) { setenv(name, value, 1); }
static void unsetEnv(const char *name) { unsetenv(name); }
#endif
#include <ctype.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>
#include <cmath>
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

/* ------------------------------------------------------------------------ */
/* Who is running: Rexx for Python, or Python (docs/design.md, *Signals*)     */
/* ------------------------------------------------------------------------ */

// rexxBusy counts the threads running Rexx code on behalf of Python: a
// thread enters it in intoRexx, leaves it while Python code runs for Rexx
// (the Gil below), and comes back when that Python code returns.  inRexx
// says whether this thread is counted now.  A Ctrl-C in a Python host
// halts Rexx when rexxBusy is not 0, and is Python's otherwise.
static std::atomic<int> rexxBusy{0};
static thread_local bool inRexx = false;

class RexxRunning
{
  public:
    RexxRunning() : was(inRexx) { if (!was) { inRexx = true; rexxBusy++; } }
    ~RexxRunning() { end(); }
    void end() { if (active) { active = false; if (!was) { inRexx = false; rexxBusy--; } } }
  private:
    bool was;
    bool active = true;
};

class Gil
{
  public:
    Gil() : was(inRexx)
    {
        if (was) { inRexx = false; rexxBusy--; }   // Python runs now
        state = PyGILState_Ensure();
    }
    ~Gil()
    {
        PyGILState_Release(state);
        if (was) { inRexx = true; rexxBusy++; }    // back to Rexx
    }
  private:
    bool was;
    PyGILState_STATE state;
};

#ifndef _WIN32
/*
 * Python host on Unix: ooRexx's first RexxCreateInterpreter takes SIGINT,
 * SIGTERM and SIGHUP from the host (it tests whether they were set from
 * outside, but only SIGHUP's answer counts: ooRexx bug, unreported; its
 * SystemInterpreter::startInterpreter).  Python would lose its KeyboardInterrupt, and SIGTERM would no
 * longer end the process.  So every instance is created between a save and
 * a restore of the three: SIGTERM and SIGHUP go back as they were, and
 * SIGINT gets onSigint, which halts Rexx while Rexx code runs for Python
 * (with ooRexx's own handler, as ooRexx would have done, or with Halt() if
 * ooRexx installed none) and passes the signal to Python's handler
 * otherwise.  Not used when Rexx is the host: ooRexx owns the signals then.
 */
static struct sigaction hostSigint;       // the host's SIGINT action (Python's)
static struct sigaction rexxSigint;       // ooRexx's, if it installed one
static volatile sig_atomic_t haveRexxSigint = 0;
static const int haltSlots = 64;
static std::atomic<RexxInstance *> haltable[haltSlots];   // our live instances
static std::mutex signalsLock;            // instance creation, one at a time

static void runAction(const struct sigaction &a, int sig, siginfo_t *info, void *uc)
{
    if (a.sa_flags & SA_SIGINFO)
    {
        if (a.sa_sigaction != NULL) a.sa_sigaction(sig, info, uc);
    }
    else if (a.sa_handler == SIG_DFL)
    {
        sigaction(sig, &a, NULL);         // what the host had: end the process
        raise(sig);
    }
    else if (a.sa_handler != SIG_IGN && a.sa_handler != NULL) a.sa_handler(sig);
}

static void onSigint(int sig, siginfo_t *info, void *uc)
{
    if (rexxBusy.load() > 0)
    {
        if (haveRexxSigint) runAction(rexxSigint, sig, info, uc);
        else
        {
            for (int i = 0; i < haltSlots; i++)
            {
                RexxInstance *r = haltable[i].load();
                if (r != NULL) r->Halt();
            }
        }
        return;
    }
    runAction(hostSigint, sig, info, uc);
}

static bool isOnSigint(const struct sigaction &a)
{
    return (a.sa_flags & SA_SIGINFO) && a.sa_sigaction == onSigint;
}

// around RexxCreateInterpreter, with signalsLock held
struct HostSignals
{
    struct sigaction saved[3];
    static int which(int i) { static const int s[3] = {SIGINT, SIGTERM, SIGHUP}; return s[i]; }
    HostSignals() { for (int i = 0; i < 3; i++) sigaction(which(i), NULL, &saved[i]); }
    void restore()
    {
        struct sigaction now;
        sigaction(SIGINT, NULL, &now);
        if (!isOnSigint(now) && !(now.sa_flags & SA_SIGINFO ? now.sa_sigaction == saved[0].sa_sigaction
                                                             : now.sa_handler == saved[0].sa_handler))
        {
            rexxSigint = now;             // ooRexx took SIGINT: its handler, for halting Rexx
            haveRexxSigint = 1;
        }
        if (!isOnSigint(saved[0])) hostSigint = saved[0];   // the host's, as it is now
        for (int i = 1; i < 3; i++) sigaction(which(i), &saved[i], NULL);
        struct sigaction ours;
        memset(&ours, 0, sizeof(ours));
        ours.sa_sigaction = onSigint;
        ours.sa_flags = SA_SIGINFO;       // no SA_RESTART, as ooRexx's
        sigfillset(&ours.sa_mask);
        sigaction(SIGINT, &ours, NULL);
    }
};

static void haltableAdd(RexxInstance *r)
{
    for (int i = 0; i < haltSlots; i++)
    {
        RexxInstance *empty = NULL;
        if (haltable[i].compare_exchange_strong(empty, r)) return;
    }
}

static void haltableRemove(RexxInstance *r)
{
    for (int i = 0; i < haltSlots; i++)
    {
        RexxInstance *expected = r;
        if (haltable[i].compare_exchange_strong(expected, NULL)) return;
    }
}
#else
/*
 * Python host on Windows: loading rexx.dll registers ooRexx's console control
 * handler, which is called before Python's (the C runtime's).  It halts Rexx
 * on Ctrl-C, but it also takes the Ctrl-C while Python runs (lost, or a
 * crash before the first instance: there are no instances to halt yet), and
 * it ignores Ctrl-Break.  So, once rexx.dll is loaded, the bridge registers
 * its own handler, which is called first: a Ctrl-C while Rexx code runs for
 * Python goes on to ooRexx's handler (a HALT); any other Ctrl-C, and every
 * Ctrl-Break, is given to Python's handler for SIGINT or SIGBREAK, in the C
 * runtime that Python and the bridge share, as the runtime's own console
 * handler would; with no handler (SIG_DFL), the process ends, as the
 * system's default does.  The other events go on as before.
 */
static BOOL WINAPI onConsoleCtrl(DWORD type)
{
    int sig;
    if (type == CTRL_C_EVENT)
    {
        if (rexxBusy.load() > 0) return FALSE;   // ooRexx's handler: a HALT
        sig = SIGINT;
    }
    else if (type == CTRL_BREAK_EVENT) sig = SIGBREAK;
    else return FALSE;
    void (*handler)(int) = signal(sig, SIG_IGN);  // Python's, as it is now
    signal(sig, handler);
    if (handler == SIG_IGN) return TRUE;
    if (handler == SIG_DFL || handler == SIG_ERR) ExitProcess(STATUS_CONTROL_C_EXIT);
    handler(sig);
    return TRUE;
}
#endif

/* ------------------------------------------------------------------------ */
/* Val: a value between the two runtimes, owned by neither lock               */
/* ------------------------------------------------------------------------ */

struct Val
{
    enum Kind { NIL, STR, REF, LIST, REXX, LINK, TOO_DEEP } kind = NIL;
    std::string s;                        // STR: the bytes
    PyObject *ref = NULL;                 // REF: a Python object
                                          // REXX: its Python proxy, if it came from Python
    bool owned = false;                   // REF/REXX: we hold a reference to ref
    std::vector<Val> items;               // LIST
    size_t id = 0;                        // LIST: its number in its conversion (from 1);
                                          // LINK: the LIST it stands for (an Array met
                                          // again: a cycle, or an Array that appears twice)
    RexxObjectPtr robj = NULLOBJECT;      // REXX: a Rexx object
    struct Inst *inst = NULL;             // REXX: its interpreter instance
    bool boolean = false;                 // STR: a boolean ("1"/"0"): Python's
                                          // bool, Rexx's .LogicalWrapper (*Values*)
    unsigned long keepKey = 0;            // REXX: robj is kept alive under this key
};

// a Python error, captured with the GIL and raised in Rexx without it
struct PyError
{
    bool set = false;
    std::string message;                  // "ValueError: ..."
    std::string traceback;
    PyObject *exc = NULL;                 // the exception itself (owned), for
                                          // raising it again as itself in Python
    PyError() {}
    PyError(const PyError &) = delete;
    PyError &operator=(const PyError &) = delete;
    ~PyError();
};

// what the bridge needs from the Rexx side during one call
struct RexxSide
{
    RexxThreadContext *tc;
    RexxInstance *inst;
    RexxClassObject pyObjectClass;
    RexxClassObject kwClass;
    RexxDirectoryObject keep = NULLOBJECT;
    RexxClassObject booleanClass = NULLOBJECT;   // ooRexx's .LogicalWrapper, and
    RexxObjectPtr trueObj = NULLOBJECT;          // its two instances
    RexxObjectPtr falseObj = NULLOBJECT;
    bool strings = false;                 // the receiver came from a module imported
                                          // as "string" (docs/design.md, *Values*)
};

/*
 * While a .PyObject reached through a module imported as "string" converts
 * its arguments, a Rexx string goes as a str, whatever it looks like.  Set by
 * each method around its own conversions, and cleared where Rexx values go to
 * Python for other reasons (Rexx results returned to Python).
 */
static thread_local bool argStrings = false;

struct StringsScope
{
    bool saved;
    explicit StringsScope(bool on) : saved(argStrings) { argStrings = on; }
    ~StringsScope() { argStrings = saved; }
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
    RexxClassObject pyObjectClass = NULLOBJECT, kwClass = NULLOBJECT;
    RexxClassObject booleanClass = NULLOBJECT;    // .LogicalWrapper (logicalwrapper.cls,
    RexxObjectPtr trueObj = NULLOBJECT;           // which python.cls requires) and
    RexxObjectPtr falseObj = NULLOBJECT;          // its two instances
    RexxDirectoryObject keep = NULLOBJECT;
    RexxObjectPtr graveyard;              // .PyObject~graveyard (see drainGraveyard)
    RexxClassObject commandHandlerClass = NULLOBJECT;     // ooRexx's .CommandHandler
    RexxClassObject pyCommandHandlerClass = NULLOBJECT;   // python.cls's .PyCommandHandler
    RexxRoutineObject closing = NULLOBJECT;               // python.cls's py.closing
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

// .LogicalWrapper and its two instances into d (global references, once per
// instance); without logicalwrapper.cls, booleans cross as "1" and "0"
static void setBooleans(RexxThreadContext *tc, InstanceData &d, RexxClassObject cls)
{
    if (cls == NULLOBJECT) return;
    RexxObjectPtr t = tc->SendMessage0(cls, "TRUE");
    RexxObjectPtr f = t != NULLOBJECT ? tc->SendMessage0(cls, "FALSE") : NULLOBJECT;
    if (tc->CheckCondition()) tc->ClearCondition();
    if (t == NULLOBJECT || f == NULLOBJECT) return;
    d.booleanClass = (RexxClassObject)tc->RequestGlobalReference(cls);
    d.trueObj = tc->RequestGlobalReference(t);
    d.falseObj = tc->RequestGlobalReference(f);
}

static bool findClasses(RexxMethodContext *context, RexxSide &side)
{
    side.tc = context->threadContext;
    side.inst = context->threadContext->instance;
    side.pyObjectClass = context->FindContextClass("PYOBJECT");
    side.kwClass = context->FindContextClass("PYKWARGS");
    RexxObjectPtr strings = context->GetObjectVariable("PYSTRINGS");
    side.strings = strings != NULLOBJECT && strings == context->True();
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
            setBooleans(side.tc, d, context->FindContextClass("LOGICALWRAPPER"));
            d.keep = (RexxDirectoryObject)side.tc->RequestGlobalReference(side.tc->NewDirectory());
            d.graveyard = graveyardOf(side.tc, side.pyObjectClass);
            instances[side.inst] = d;
            side.keep = d.keep;
            side.booleanClass = d.booleanClass;
            side.trueObj = d.trueObj;
            side.falseObj = d.falseObj;
            graveyard = d.graveyard;
        }
        else
        {
            side.keep = it->second.keep;
            side.booleanClass = it->second.booleanClass;
            side.trueObj = it->second.trueObj;
            side.falseObj = it->second.falseObj;
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
    side.booleanClass = it->second.booleanClass;
    side.trueObj = it->second.trueObj;
    side.falseObj = it->second.falseObj;
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

// pairs: a Rexx object and the Python object of a Rexx class that extends a
// Python class (defined below, *Pairs*)
static PyObject *pairType = NULL;         // oorexx._Pair, a base of every such Python class
static unsigned long long pairId(PyObject *o);
static bool pairCross(unsigned long long id, RexxObjectPtr *r, Inst **inst);

/* phase 1: Rexx -> Val (no GIL) ------------------------------------------ */

// the items of a multi-dimensional Array (flat, as ArrayAt numbers them: the
// first index varies fastest) as lists nested by dimension, the first
// outermost: out[i1][i2]... is flat[i1 + d1 * (i2 + d2 * ...)]
static void nestLevel(std::vector<Val> &flat, const std::vector<size_t> &dims,
                      const std::vector<size_t> &stride, size_t level, size_t base, Val &out)
{
    out.kind = Val::LIST;
    out.items.resize(dims[level]);
    for (size_t k = 0; k < dims[level]; k++)
    {
        size_t at = base + k * stride[level];
        if (level + 1 == dims.size()) std::swap(out.items[k], flat[at]);
        else nestLevel(flat, dims, stride, level + 1, at, out.items[k]);
    }
}

static void nest(Val &v, const std::vector<size_t> &dims)
{
    std::vector<size_t> stride(dims.size(), 1);
    for (size_t k = 1; k < dims.size(); k++) stride[k] = stride[k - 1] * dims[k - 1];
    std::vector<Val> flat;
    flat.swap(v.items);
    nestLevel(flat, dims, stride, 0, 0, v);
}

/*
 * One value is converted with a memo of the Arrays already copied, so that
 * the copy has the structure of the original: an Array met again (one that
 * contains itself, Arrays that contain each other, an Array that appears
 * twice) is a LINK to the list made for it (docs/design.md, *Values*). The
 * copy recurses in C, so Arrays nested deeper than maxArrayDepth are an
 * error.
 */
static const size_t maxArrayDepth = 1000;

struct ArrayMemo
{
    std::map<RexxObjectPtr, size_t> ids;  // Array -> its LIST's id
    size_t depth = 0;
};

static void rexxToVal(RexxSide &side, RexxObjectPtr o, Val &v, ArrayMemo &memo);

// the text of any Rexx object, as its STRING method gives it.  Not
// ObjectToStringValue / ObjectToString on an object that is not a string:
// they go through the interpreter's requestString, which raises NOSTRING in
// the Rexx caller when it traps that condition (as Rexx does when it uses
// such an object as a string), and the bridge taking a name or a value's
// text is not that.
static std::string textOf(RexxThreadContext *c, RexxObjectPtr o)
{
    if (o == NULLOBJECT) return std::string();
    if (c->IsString(o))
        return std::string(c->StringData((RexxStringObject)o), c->StringLength((RexxStringObject)o));
    RexxObjectPtr s = c->SendMessage0(o, "STRING");
    std::string text;
    if (s != NULLOBJECT)
    {
        if (c->IsString(s)) text.assign(c->StringData((RexxStringObject)s), c->StringLength((RexxStringObject)s));
        else text = c->ObjectToStringValue(s);     // a string kept in another form (an integer)
        if (s != o) c->ReleaseLocalReference(s);
    }
    return text;
}

// whether o is an instance of one of the bridge's classes (.PyObject,
// .PyKwArgs, .LogicalWrapper)
static bool instanceOf(RexxThreadContext *c, RexxObjectPtr o, RexxClassObject cls)
{
    if (o == NULLOBJECT || cls == NULLOBJECT) return false;
    return c->IsInstanceOf(o, cls) != 0;
}

static void rexxToVal(RexxSide &side, RexxObjectPtr o, Val &v)
{
    ArrayMemo memo;
    rexxToVal(side, o, v, memo);
}

static void rexxToVal(RexxSide &side, RexxObjectPtr o, Val &v, ArrayMemo &memo)
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
        if ((RexxObjectPtr)str != o) c->ReleaseLocalReference(str);
    }
    // not a String from here on, so IsInstanceOf is safe (see instanceOf)
    else if (side.booleanClass != NULLOBJECT && c->IsInstanceOf(o, side.booleanClass))
    {
        // a .LogicalWrapper (or a subclass's: JSON's, YAML's): Python's bool
        // (docs/design.md, *Values*)
        v.kind = Val::STR;
        v.s = c->ObjectToStringValue(o);
        v.boolean = true;
    }
    else if (c->IsInstanceOf(o, side.pyObjectClass))
    {
        RexxObjectPtr p = c->SendMessage0(o, "PY.POINTER");
        v.kind = Val::REF;
        v.ref = p != NULLOBJECT ? (PyObject *)c->PointerValue((RexxPointerObject)p) : NULL;
        v.owned = false;                  // the proxy keeps it alive during the call
        if (p != NULLOBJECT) c->ReleaseLocalReference(p);
        if (v.ref == NULL) v.kind = Val::NIL;
    }
    else if (c->IsArray(o))
    {
        auto seen = memo.ids.find(o);
        if (seen != memo.ids.end())
        {
            v.kind = Val::LINK;
            v.id = seen->second;
            return;
        }
        if (memo.depth >= maxArrayDepth)
        {
            v.kind = Val::TOO_DEEP;
            return;
        }
        v.kind = Val::LIST;
        v.id = memo.ids.size() + 1;
        memo.ids[o] = v.id;
        RexxArrayObject a = (RexxArrayObject)o;
        size_t n = c->ArraySize(a);
        v.items.resize(n);
        memo.depth++;
        // each item is a local reference of the native method in progress,
        // kept until it returns unless released: released once copied, so a
        // big Array costs no more than its copy (an Array of 1,000,000
        // strings sent three times grew the process by 161 MB otherwise)
        for (size_t i = 1; i <= n; i++)
        {
            RexxObjectPtr item = c->ArrayAt(a, i);
            rexxToVal(side, item, v.items[i - 1], memo);
            if (item != NULLOBJECT) c->ReleaseLocalReference(item);
        }
        memo.depth--;
        // a multi-dimensional Array: nested lists, by dimension, the first
        // outermost (docs/design.md, *Values*)
        if (c->ArrayDimension(a) > 1)
        {
            RexxObjectPtr d = c->SendMessage0(a, "DIMENSIONS");
            std::vector<size_t> dims;
            if (d != NULLOBJECT && c->IsArray(d))
            {
                size_t k = c->ArrayItems((RexxArrayObject)d);
                for (size_t i = 1; i <= k; i++)
                {
                    wholenumber_t w = 0;
                    RexxObjectPtr x = c->ArrayAt((RexxArrayObject)d, i);
                    c->ObjectToWholeNumber(x, &w);
                    if (x != NULLOBJECT) c->ReleaseLocalReference(x);
                    dims.push_back((size_t)w);
                }
            }
            if (d != NULLOBJECT) c->ReleaseLocalReference(d);
            size_t total = 1;
            for (size_t x : dims) total *= x;
            if (dims.size() > 1 && total == n) nest(v, dims);
        }
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
        if (instanceOf(c, tail, side.kwClass))
        {
            last = n - 1;
            RexxArrayObject names = (RexxArrayObject)c->SendMessage0(tail, "NAMES");
            RexxArrayObject values = (RexxArrayObject)c->SendMessage0(tail, "VALUES");
            size_t k = c->ArraySize(names);
            for (size_t i = 1; i <= k; i++)
            {
                std::pair<std::string, Val> kv;
                kv.first = textOf(c, c->ArrayAt(names, i));
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

/*
 * A float's text in Rexx (docs/design.md, *Values*), from Python's repr (the
 * shortest digits that read back as the same double): what ooRexx writes for
 * x + 0 under NUMERIC DIGITS 17, that is, plain digits unless the integer
 * part needs more than 17 places or the decimals more than 34, otherwise
 * scientific (1E+17, 1.5E-35); a ".0" on a whole number written plain, so
 * that it goes back to Python as a float (the number rule reads the point);
 * -0.0 kept; NaN and the infinities as ooRexx writes them: nan, +infinity,
 * -infinity.
 */
static std::string rexxFloatText(const std::string &r)
{
    // ooRexx's own spelling (what DoubleToObject writes, ObjectToDouble reads)
    if (r == "nan" || r == "-nan") return "nan";
    if (r == "inf") return "+infinity";
    if (r == "-inf") return "-infinity";
    std::string sign, digits;
    long exp = 0;                         // the value is digits * 10**exp
    size_t i = 0;
    if (i < r.size() && r[i] == '-') { sign = "-"; i++; }
    bool point = false;
    for (; i < r.size() && r[i] != 'e' && r[i] != 'E'; i++)
    {
        if (r[i] == '.') point = true;
        else
        {
            digits += r[i];
            if (point) exp--;
        }
    }
    if (i < r.size()) exp += strtol(r.c_str() + i + 1, NULL, 10);
    size_t lead = digits.find_first_not_of('0');
    if (lead == std::string::npos) return sign + "0.0";       // 0.0, -0.0
    digits.erase(0, lead);
    size_t last = digits.find_last_not_of('0');
    exp += (long)(digits.size() - 1 - last);
    digits.erase(last + 1);
    long n = (long)digits.size();
    const long rexxDigits = 17;
    if (n + exp > rexxDigits || -exp > 2 * rexxDigits)
    {
        long e = n + exp - 1;
        std::string out = sign + digits.substr(0, 1);
        if (n > 1) out += "." + digits.substr(1);
        return out + "E" + (e < 0 ? "-" : "+") + std::to_string(e < 0 ? -e : e);
    }
    if (exp >= 0) return sign + digits + std::string((size_t)exp, '0') + ".0";
    if (n + exp > 0) return sign + digits.substr(0, (size_t)(n + exp)) + "." + digits.substr((size_t)(n + exp));
    return sign + "0." + std::string((size_t)(-(n + exp)), '0') + digits;
}

// is d, the double of the Rexx number s, a value of another kind: not
// finite, or zero while s is not?
static bool floatChangesKind(double d, const std::string &s)
{
    if (!std::isfinite(d)) return true;
    if (d != 0.0) return false;
    for (char ch : s)
    {
        if (ch == 'e' || ch == 'E') break;
        if (ch >= '1' && ch <= '9') return true;
    }
    return false;
}

// a new reference, or NULL with a Python error set; lists: the lists made so
// far in this conversion, by id (borrowed: the result holds them)
static PyObject *valToPy(const Val &v, std::vector<PyObject *> &lists);

static PyObject *valToPy(const Val &v)
{
    std::vector<PyObject *> lists;
    return valToPy(v, lists);
}

static PyObject *valToPy(const Val &v, std::vector<PyObject *> &lists)
{
    switch (v.kind)
    {
        case Val::LINK:
            if (v.id == 0 || v.id > lists.size() || lists[v.id - 1] == NULL)
            {
                PyErr_SetString(PyExc_SystemError, "ooRexx/Python: a Rexx Array met again could not be found");
                return NULL;
            }
            Py_INCREF(lists[v.id - 1]);
            return lists[v.id - 1];
        case Val::TOO_DEEP:
            PyErr_Format(PyExc_ValueError,
                "ooRexx/Python: Rexx Arrays nested more than %d deep cannot be copied", (int)maxArrayDepth);
            return NULL;
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
            if (v.id != 0)
            {
                if (lists.size() < v.id) lists.resize(v.id, NULL);
                lists[v.id - 1] = list;
            }
            for (size_t i = 0; i < v.items.size(); i++)
            {
                PyObject *item = valToPy(v.items[i], lists);
                if (item == NULL) { Py_DECREF(list); return NULL; }
                PyList_SetItem(list, (Py_ssize_t)i, item);
            }
            return list;
        }
        case Val::STR:
        default:
            if (v.boolean) return PyBool_FromLong(v.s == "1");
            if (argStrings) return pyText(v.s.data(), v.s.size());
            switch (numberKind(v.s))
            {
                case INTEGER_NUMBER: return PyLong_FromString(v.s.c_str(), NULL, 10);
                case FLOAT_NUMBER:
                {
                    PyObject *text = PyUnicode_FromString(v.s.c_str());
                    if (text == NULL) return NULL;
                    PyObject *f = PyFloat_FromString(text);
                    // a float of another kind (overflow: inf; underflow: 0.0
                    // for a number that is not zero) would lose the value
                    // silently: decimal.Decimal, exact (docs/design.md, *Values*)
                    if (f != NULL && floatChangesKind(PyFloat_AsDouble(f), v.s))
                    {
                        Py_DECREF(f);
                        PyObject *m = PyImport_ImportModule("decimal");
                        PyObject *d = m != NULL ? PyObject_GetAttrString(m, "Decimal") : NULL;
                        f = d != NULL ? PyObject_CallFunctionObjArgs(d, text, NULL) : NULL;
                        Py_XDECREF(d);
                        Py_XDECREF(m);
                    }
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
    if (pairType != NULL && PyObject_TypeCheck(o, (PyTypeObject *)pairType))
    {
        // the Python object of a pair: its Rexx object, itself (a crossing;
        // our reference to o is dropped once Rexx has it)
        unsigned long long id = pairId(o);
        RexxObjectPtr r;
        Inst *inst;
        if (id != 0 && pairCross(id, &r, &inst))
        {
            v.kind = Val::REXX;
            v.robj = r;
            v.inst = inst;
            v.ref = o;
            v.owned = true;
            return;
        }
        keep = true;                      // its pair is over: a plain proxy
    }
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
            v.boolean = true;
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
                if (PyFloat_Check(o)) v.s = rexxFloatText(v.s);
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
    if (value != NULL)
    {
        if (tb != NULL) PyException_SetTraceback(value, tb);
        Py_XDECREF(err.exc);
        Py_INCREF(value);
        err.exc = value;
    }
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

PyError::~PyError()
{
    if (exc != NULL)
    {
        Gil gil;                          // nests if this thread holds it
        Py_DECREF(exc);
    }
}

// a bridge error (not raised by Python code), Python-style
static void bridgeError(PyError &err, const std::string &message)
{
    err.set = true;
    err.message = message;
}

/*
 * Parameters annotated bool (docs/design.md, *Values*): a Rexx "1" or "0"
 * passed to one goes as True or False.  The annotations are read by a small
 * Python function, compiled once: only Python functions, methods and classes
 * are looked at (C functions have no annotations), each annotation alone (a
 * type, or a string under `from __future__ import annotations`; Python 3.14
 * is asked for strings), and only `bool`, `bool | None`,
 * `Optional[bool]` and `Union[bool, None]` count, `bool` being also any name that the function's
 * module binds to bool (pandas 2 writes `bool_t`).  The answer is cached per function; any failure
 * means "no bool parameters".
 */
static PyObject *builtinsModule();

static const char *boolParamsSource =
    "import inspect, sys, typing, types\n"
    "_cache = {}\n"
    "_kw = {}\n"
    "if sys.version_info >= (3, 14):\n"
    "    import annotationlib\n"
    "    _kw = {'annotation_format': annotationlib.Format.STRING}\n"
    "_unions = (typing.Union, getattr(types, 'UnionType', None))\n"
    "def _is_bool(a, g):\n"
    "    if a is bool:\n"
    "        return True\n"
    "    if isinstance(a, str):\n"
    "        s = a.replace(' ', '').replace('typing.', '')\n"
    "        if s.startswith('Optional[') and s.endswith(']'):\n"
    "            s = s[9:-1]\n"
    "        elif s.startswith('Union[') and s.endswith(']'):\n"
    "            s = s[6:-1].replace(',', '|')\n"
    "        rest = [p for p in s.split('|') if p not in ('None', 'NoneType')]\n"
    "        if len(rest) != 1 or not rest[0].isidentifier():\n"
    "            return False\n"
    "        return rest[0] == 'bool' or g.get(rest[0]) is bool\n"
    "    if typing.get_origin(a) in _unions:\n"
    "        rest = [x for x in typing.get_args(a) if x is not type(None)]\n"
    "        return rest == [bool]\n"
    "    return False\n"
    "def bool_params(f):\n"
    "    if inspect.ismethod(f):\n"
    "        key = (f.__func__, 1)\n"
    "        code = f.__func__\n"
    "    elif inspect.isfunction(f):\n"
    "        key = (f, 0)\n"
    "        code = f\n"
    "    elif inspect.isclass(f):\n"
    "        key = (f, 0)\n"
    "        code = getattr(f, '__init__', None)\n"
    "    else:\n"
    "        return None\n"
    "    try:\n"
    "        return _cache[key]\n"
    "    except (KeyError, TypeError):\n"
    "        pass\n"
    "    r = None\n"
    "    try:\n"
    "        ps = inspect.signature(f, **_kw).parameters.values()\n"
    "        names = tuple(p.name for p in ps if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD))\n"
    "        g = getattr(inspect.unwrap(code), '__globals__', {})\n"
    "        bools = frozenset(p.name for p in ps if _is_bool(p.annotation, g))\n"
    "        if bools:\n"
    "            r = (names, bools)\n"
    "    except Exception:\n"
    "        pass\n"
    "    try:\n"
    "        if len(_cache) > 10000:\n"
    "            _cache.clear()\n"
    "        _cache[key] = r\n"
    "    except TypeError:\n"
    "        pass\n"
    "    return r\n";

// (names of the positional parameters, set of the bool ones), or NULL
static PyObject *boolParams(PyObject *f)
{
    static PyObject *helper = NULL;        // with the GIL: no race
    if (helper == NULL)
    {
        PyObject *b = builtinsModule();
        PyObject *globals = PyDict_New();
        PyObject *r = NULL;
        if (b != NULL && globals != NULL && PyDict_SetItemString(globals, "__builtins__", b) == 0)
            r = PyObject_CallMethod(b, "exec", "sO", boolParamsSource, globals);
        if (r != NULL) helper = PyDict_GetItemString(globals, "bool_params");
        Py_XINCREF(helper);
        Py_XDECREF(r);
        Py_XDECREF(globals);
        Py_XDECREF(b);
        PyErr_Clear();
        if (helper == NULL) { helper = Py_None; Py_INCREF(helper); }   // never again
    }
    if (helper == Py_None) return NULL;
    PyObject *r = PyObject_CallFunctionObjArgs(helper, f, NULL);
    if (r == NULL) { PyErr_Clear(); return NULL; }
    if (r == Py_None) { Py_DECREF(r); return NULL; }
    return r;
}

/*
 * A Rexx number asked for explicitly as a float (.py~float(x) with a Rexx
 * string, .py~box("float"/"complex", x)) that does not fit a double is an
 * error, not an infinity or a zero (docs/design.md, *Values*): OverflowError
 * when it would be infinite, ValueError when it is not zero and would be
 * zero. By name (+infinity, -infinity, nan, and Python's inf, Infinity, NaN)
 * still works; excess digits still round. A small Python function, compiled
 * once, compares each part of the result with the exact decimal value of its
 * text.
 */
static const char *floatRangeSource =
    "import decimal\n"
    "def _part(x, t, text, what):\n"
    "    if x != x or (x != 0.0 and x not in (float('inf'), float('-inf'))):\n"
    "        return\n"
    "    try:\n"
    "        d = decimal.Decimal(t.strip())\n"
    "    except Exception:\n"
    "        return\n"
    "    if x == 0.0:\n"
    "        if not (d.is_zero() or d.is_nan()):\n"
    "            raise ValueError('ooRexx/Python: ' + text + ' is out of range for a ' + what + ' (it would be 0)')\n"
    "    elif not d.is_infinite():\n"
    "        raise OverflowError('ooRexx/Python: ' + text + ' is out of range for a ' + what)\n"
    "def float_range(r, text):\n"
    "    if type(r) is float:\n"
    "        _part(r, text, text, 'float')\n"
    "    elif type(r) is complex:\n"
    "        t = text.strip()\n"
    "        if t[:1] == '(' and t[-1:] == ')':\n"
    "            t = t[1:-1].strip()\n"
    "        if t[-1:] in ('j', 'J'):\n"
    "            body = t[:-1]\n"
    "            k = max([i for i, c in enumerate(body) if c in '+-' and i > 0 and body[i - 1] not in 'eE'] or [-1])\n"
    "            re_t, im_t = ('0', body) if k < 0 else (body[:k], body[k:])\n"
    "            if im_t in ('', '+', '-'):\n"
    "                im_t += '1'\n"
    "        else:\n"
    "            re_t, im_t = t, '0'\n"
    "        _part(r.real, re_t, text, 'complex')\n"
    "        _part(r.imag, im_t, text, 'complex')\n";

// false, with the Python error set, if r (a float or complex made from the
// Rexx text) lost the range of the number
static bool floatInRange(PyObject *r, const std::string &text)
{
    static PyObject *helper = NULL;        // with the GIL: no race
    if (helper == NULL)
    {
        PyObject *b = builtinsModule();
        PyObject *globals = PyDict_New();
        PyObject *x = NULL;
        if (b != NULL && globals != NULL && PyDict_SetItemString(globals, "__builtins__", b) == 0)
            x = PyObject_CallMethod(b, "exec", "sO", floatRangeSource, globals);
        if (x != NULL) helper = PyDict_GetItemString(globals, "float_range");
        Py_XINCREF(helper);
        Py_XDECREF(x);
        Py_XDECREF(globals);
        Py_XDECREF(b);
        PyErr_Clear();
        if (helper == NULL) { helper = Py_None; Py_INCREF(helper); }
    }
    if (helper == Py_None || r == NULL || !(PyFloat_CheckExact(r) || PyComplex_CheckExact(r))) return true;
    PyObject *t = pyText(text.data(), text.size());
    if (t == NULL) { PyErr_Clear(); return true; }
    PyObject *x = PyObject_CallFunctionObjArgs(helper, r, t, NULL);
    Py_DECREF(t);
    if (x == NULL) return false;
    Py_DECREF(x);
    return true;
}

static bool isRexxBool(const Val &v)
{
    return v.kind == Val::STR && (v.s == "1" || v.s == "0");
}

// v as Python, True/False if it is "1"/"0" going to a bool parameter
static PyObject *argToPy(const Val &v, PyObject *bools, PyObject *name)
{
    if (bools != NULL && name != NULL && isRexxBool(v) && PySet_Contains(bools, name) == 1)
    {
        PyObject *b = v.s == "1" ? Py_True : Py_False;
        Py_INCREF(b);
        return b;
    }
    return valToPy(v);
}

// the positional arguments as a tuple and the keywords as a dict (or NULL);
// with f, its bool parameters are honoured (boolParams)
static bool buildArgs(const Args &a, PyObject **tuple, PyObject **dict, PyObject *f = NULL)
{
    PyObject *info = NULL, *names = NULL, *bools = NULL;
    if (f != NULL)
    {
        bool any = false;
        for (const Val &v : a.positional) any = any || isRexxBool(v);
        for (const auto &kv : a.keywords) any = any || isRexxBool(kv.second);
        if (any) info = boolParams(f);
        if (info != NULL)
        {
            names = PyTuple_GetItem(info, 0);
            bools = PyTuple_GetItem(info, 1);
        }
    }
    struct Drop { PyObject *o; ~Drop() { Py_XDECREF(o); } } drop{info};
    *dict = NULL;
    *tuple = PyTuple_New((Py_ssize_t)a.positional.size());
    if (*tuple == NULL) return false;
    for (size_t i = 0; i < a.positional.size(); i++)
    {
        PyObject *name = names != NULL && (Py_ssize_t)i < PyTuple_Size(names)
                       ? PyTuple_GetItem(names, (Py_ssize_t)i) : NULL;
        PyObject *item = argToPy(a.positional[i], bools, name);
        if (item == NULL) { Py_CLEAR(*tuple); return false; }
        PyTuple_SetItem(*tuple, (Py_ssize_t)i, item);
    }
    if (!a.keywords.empty())
    {
        *dict = PyDict_New();
        for (const auto &kv : a.keywords)
        {
            PyObject *name = bools != NULL ? PyUnicode_FromString(kv.first.c_str()) : NULL;
            PyObject *value = argToPy(kv.second, bools, name);
            Py_XDECREF(name);
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
    if (!buildArgs(a, &tuple, &dict, f)) { captureError(err); return; }
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
            // a bool: .LogicalWrapper~true or ~false (docs/design.md, *Values*);
            // "1"/"0" where logicalwrapper.cls is not loaded
            if (v.boolean && side.trueObj != NULLOBJECT) return v.s == "1" ? side.trueObj : side.falseObj;
            return c->String(v.s.data(), v.s.size());
        case Val::REF:
        {
            // the proxy takes over our reference (its UNINIT drops it)
            RexxObjectPtr proxy = side.strings
                ? c->SendMessage2(side.pyObjectClass, "NEW", c->NewPointer(v.ref), c->True())
                : c->SendMessage1(side.pyObjectClass, "NEW", c->NewPointer(v.ref));
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

static PyObject *rexxErrorType = NULL;   // oorexx.RexxError

// an oorexx.RexxError made from a Rexx condition of this instance (it keeps
// the condition object, raiseRexxError): raise that condition again, as
// itself, with its code and ADDITIONAL (SYNTAX), or its DESCRIPTION,
// ADDITIONAL and RESULT (any other), as Rexx would see it with no Python in
// between.  Whether it did; *halted when it was a HALT (raised by halting the
// thread, so the call still returns a value: see below).
static bool raiseAgain(RexxSide &side, PyError &err, bool *halted)
{
    *halted = false;
    RexxObjectPtr cond = NULLOBJECT;
    {
        Gil gil;
        if (err.exc != NULL && PyObject_IsInstance(err.exc, rexxErrorType) == 1)
        {
            PyObject *p = PyObject_GetAttrString(err.exc, "_rexx_condition");
            Inst *inst;
            RexxObjectPtr target;
            if (p != NULL && rexxProxyTarget(p, &inst, &target) && inst != NULL && inst->rexx == side.inst)
                cond = target;            // alive while err.exc holds the proxy
            Py_XDECREF(p);
        }
        PyErr_Clear();
    }
    RexxThreadContext *c = side.tc;
    if (cond == NULLOBJECT || !c->IsDirectory(cond)) return false;
    RexxDirectoryObject d = (RexxDirectoryObject)cond;
    RexxObjectPtr nameObj = c->DirectoryAt(d, "CONDITION");
    if (nameObj == NULLOBJECT) return false;
    std::string name = upper(c->ObjectToStringValue(nameObj));
    RexxObjectPtr additional = c->DirectoryAt(d, "ADDITIONAL");
    if (name == "SYNTAX")
    {
        RexxObjectPtr codeObj = c->DirectoryAt(d, "CODE");
        if (codeObj == NULLOBJECT) return false;
        std::string code = c->ObjectToStringValue(codeObj);
        size_t dot = code.find('.');
        if (dot == std::string::npos) return false;
        wholenumber_t major = atol(code.substr(0, dot).c_str()), minor = atol(code.substr(dot + 1).c_str());
        RexxArrayObject args = additional != NULLOBJECT && c->IsArray(additional) ? (RexxArrayObject)additional
                                                                                  : c->NewArray(0);
        // 4.1 "Program interrupted with %1 condition" is what ooRexx makes of
        // a HALT the Rexx code did not trap, before a native caller sees it:
        // the Rexx caller gets a HALT again (its DESCRIPTION, ADDITIONAL and
        // RESULT are already gone).  Not RaiseCondition: ooRexx ignores a
        // condition other than SYNTAX that native code raises and nobody
        // traps, and a HALT must not be lost.  HaltThread is a Ctrl-C on this
        // thread: SIGNAL ON HALT and CALL ON HALT trap it at the end of the
        // clause, and untrapped it is SYNTAX 4.1, as in plain Rexx.  The call
        // returns .nil meanwhile (CALL ON HALT resumes the clause).
        if (major == 4 && minor == 1)
        {
            c->HaltThread();
            *halted = true;
            return true;
        }
        c->RaiseException((size_t)(major * 1000 + minor), args);
        return true;
    }
    RexxObjectPtr description = c->DirectoryAt(d, "DESCRIPTION");
    RexxObjectPtr result = c->DirectoryAt(d, "RESULT");
    c->RaiseCondition(name.c_str(), description != NULLOBJECT ? c->ObjectToString(description) : NULLOBJECT,
                      additional, result);
    return true;
}

static RexxObjectPtr finish(RexxSide &side, Val &result, PyError &err)
{
    RexxThreadContext *c = side.tc;
    bool halted;
    if (err.set && raiseAgain(side, err, &halted))
    {
        if (result.kind != Val::NIL) { Gil gil; dropVal(result); }
        return halted ? c->Nil() : NULLOBJECT;
    }
    if (err.set)
    {
        if (result.kind != Val::NIL) { Gil gil; dropVal(result); }
        RexxArrayObject extra = c->NewArray(3);
        std::string text = "Python error: " + err.message;
        if (!text.empty() && text.back() == '.') text.pop_back();   // the message adds one
        c->ArrayPut(extra, c->String(text.c_str()), 1);
        // the exception itself, as a .PyObject, in [2] and the traceback in
        // [3], as BSF4ooRexx does: if the condition goes back to Python, the
        // exception is raised again as itself (docs/design.md, *Exceptions*)
        c->ArrayPut(extra, c->String(err.traceback.c_str()), 3);
        if (err.exc != NULL && side.pyObjectClass != NULLOBJECT)
        {
            Val ev;
            ev.kind = Val::REF;
            ev.ref = err.exc;
            ev.owned = true;
            err.exc = NULL;               // the proxy takes our reference
            RexxObjectPtr eo = valToRexx(side, ev);
            if (eo != NULLOBJECT) c->ArrayPut(extra, eo, 2);
        }
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

// what an oorexx.RexxError tells about its Rexx condition, as attributes
// (taken under the kernel lock, set as attributes with the GIL)
struct ConditionInfo
{
    bool set = false;
    std::string condition, code, message, errortext, program, description, traceback;
    long rc = 0;
    size_t position = 0;
    Val object;                           // the condition object itself (REXX), so
                                          // that it can be raised again as itself
};

// a pending Rexx condition as the text of an oorexx.RexxError (and cleared),
// and its parts into ci; a Python exception that went through Rexx (SYNTAX
// 98.900 with the exception as the second ADDITIONAL item: finish,
// .py~raise) goes into exc, borrowed from its proxy, to be raised again as
// itself
static std::string takeCondition(RexxThreadContext *tc, RexxSide *side = NULL, Val *exc = NULL,
                                 ConditionInfo *ci = NULL)
{
    RexxDirectoryObject cond = tc->GetConditionInfo();
    RexxCondition info;
    tc->DecodeConditionInfo(cond, &info);
    if (side != NULL && exc != NULL && side->pyObjectClass != NULLOBJECT && info.code == 98900)
    {
        RexxObjectPtr add = tc->DirectoryAt(cond, "ADDITIONAL");
        if (add != NULLOBJECT && tc->IsArray(add) && tc->ArrayItems((RexxArrayObject)add) >= 2)
        {
            RexxObjectPtr e = tc->ArrayAt((RexxArrayObject)add, 2);
            if (instanceOf(tc, e, side->pyObjectClass)) rexxToVal(*side, e, *exc);
        }
    }
    std::string name = info.conditionName != NULLOBJECT ? tc->CString(info.conditionName) : "SYNTAX";
    std::string text = info.message != NULLOBJECT ? tc->CString(info.message) : "";
    if (text.empty() && info.errortext != NULLOBJECT) text = tc->CString(info.errortext);
    char code[64];
    snprintf(code, sizeof code, "%ld.%ld", (long)(info.code / 1000), (long)(info.code % 1000));
    if (ci != NULL)
    {
        ci->set = true;
        if (side != NULL) rexxToVal(*side, cond, ci->object);   // a Directory: by reference
        ci->condition = name;
        if (name == "SYNTAX") ci->code = code;
        ci->rc = (long)info.rc;
        ci->position = info.position;
        ci->message = text;
        if (info.errortext != NULLOBJECT) ci->errortext = tc->CString(info.errortext);
        if (info.program != NULLOBJECT) ci->program = tc->CString(info.program);
        if (info.description != NULLOBJECT) ci->description = tc->CString(info.description);
        // the Rexx traceback, as Rexx shows it (a List of lines in the
        // condition object, not in RexxCondition)
        RexxObjectPtr tb = tc->DirectoryAt(cond, "TRACEBACK");
        RexxObjectPtr lines = tb != NULLOBJECT ? tc->SendMessage0(tb, "MAKEARRAY") : NULLOBJECT;
        if (lines != NULLOBJECT && tc->IsArray(lines))
        {
            size_t n = tc->ArrayItems((RexxArrayObject)lines);
            for (size_t i = 1; i <= n; i++)
            {
                RexxObjectPtr line = tc->ArrayAt((RexxArrayObject)lines, i);
                if (line == NULLOBJECT) continue;
                if (!ci->traceback.empty()) ci->traceback += "\n";
                ci->traceback += tc->ObjectToStringValue(line);
            }
        }
    }
    tc->ClearCondition();
    if (name != "SYNTAX")
    {
        return "Rexx condition " + name + (text.empty() ? "" : ": " + text);
    }
    return std::string("Rexx error ") + code + ": " + text;
}

// raise an oorexx.RexxError with text, and ci's attributes when it has them
// (with the GIL)
static void raiseRexxError(const std::string &text, ConditionInfo &ci)
{
    PyObject *e = PyObject_CallFunction(rexxErrorType, "s", text.c_str());
    if (e == NULL) { dropVal(ci.object); return; }
    if (ci.object.kind == Val::REXX)
    {
        // the condition object, for raising it again as itself if the error
        // goes back to Rexx (raiseAgain)
        PyObject *o = valToPy(ci.object);
        if (o != NULL) { PyObject_SetAttrString(e, "_rexx_condition", o); Py_DECREF(o); }
        PyErr_Clear();
    }
    dropVal(ci.object);
    if (ci.set)
    {
        auto str = [&](const char *attr, const std::string &v, bool none)
        {
            PyObject *o = none ? Py_None : PyUnicode_DecodeUTF8(v.data(), (Py_ssize_t)v.size(), "replace");
            if (none) Py_INCREF(o);
            if (o != NULL) { PyObject_SetAttrString(e, attr, o); Py_DECREF(o); }
        };
        str("condition", ci.condition, false);
        str("code", ci.code, ci.code.empty());
        str("message", ci.message, false);
        str("errortext", ci.errortext, ci.errortext.empty());
        str("program", ci.program, ci.program.empty());
        str("description", ci.description, ci.description.empty());
        str("traceback", ci.traceback, ci.traceback.empty());
        PyObject *rc = PyLong_FromLong(ci.rc);
        if (rc != NULL) { PyObject_SetAttrString(e, "rc", rc); Py_DECREF(rc); }
        PyObject *pos = ci.position != 0 ? PyLong_FromSize_t(ci.position) : (Py_INCREF(Py_None), Py_None);
        if (pos != NULL) { PyObject_SetAttrString(e, "position", pos); Py_DECREF(pos); }
    }
    PyErr_SetObject(rexxErrorType, e);
    Py_DECREF(e);
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
    Val result, exc;
    std::string error;
    ConditionInfo ci;
    PyThreadState *saved = PyEval_SaveThread();
    RexxRunning running;                  // a Ctrl-C now halts Rexx (*Signals*)
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
        if (tc->CheckCondition()) error = takeCondition(tc, &side, &exc, &ci);
        else if (error.empty() && r != NULLOBJECT) rexxToVal(side, r, result);   // may run Rexx code
        // The Python objects in the result are borrowed from their .PyObject
        // proxies, which only this thread context's local references keep
        // alive: once it detaches, a collection can run their uninits and
        // another thread's drainGraveyard can free the objects before we take
        // the GIL back.  So take our own references now.  Waiting for the GIL
        // here is safe: between API calls this thread holds no kernel lock.
        if (holdsBorrowed(result) || holdsBorrowed(exc))
        {
            Gil gil;
            ownBorrowed(result);
            ownBorrowed(exc);
        }
        tc->DetachThread();
    }
    running.end();
    PyEval_RestoreThread(saved);

    // with the GIL: the result
    if (!error.empty())
    {
        dropVal(result);
        if (exc.kind == Val::REF && exc.ref != NULL &&
            PyObject_IsInstance(exc.ref, PyExc_BaseException) == 1)
        {
            // a Python exception that went through Rexx: itself again
            PyObject *type = PyObject_Type(exc.ref);
            PyErr_SetObject(type, exc.ref);
            Py_XDECREF(type);
            dropVal(exc);
            dropVal(ci.object);
            return NULL;
        }
        dropVal(exc);
        raiseRexxError(error, ci);
        return NULL;
    }
    PyObject *py;
    {
        StringsScope plain(false);
        py = valToPy(result);
    }
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

/* ------------------------------------------------------------------------ */
/* Pairs: Rexx classes that extend Python classes                             */
/* ------------------------------------------------------------------------ */

/*
 * docs/design.md, *Subclassing*.  An instance of a Rexx class that extends a
 * Python class (::class Worker subclass PyObject; .py~extend(self, base)) is
 * a pair: the Rexx object R and its Python object P.  R holds P (its CSELF,
 * as any .PyObject), and P reaches R through the bridge (its _oorexx_pair:
 * an entry here), for the overrides Python calls.  Each collector sees only
 * its own half, so the link that keeps the other half alive points one way
 * at a time:
 *
 *   REXX     Rexx holds R.  R holds P (a reference); the bridge knows R
 *            without holding it.  R holds a sentinel (.PyPeerSentinel) that
 *            holds R: its UNINIT runs when Rexx no longer holds R, and queues
 *            (R, id, n) on the graveyard.
 *   PYTHON   Rexx no longer holds R: the bridge holds R (the instance's keep
 *            Directory) and R no longer holds P (its pointer stays, without
 *            a reference), so Python's collector decides.  If P goes, its
 *            __del__ says so: DEAD.  If P crosses over to Rexx again (it is
 *            passed to Rexx, or Python calls an override), R holds P again:
 *   BACK     waiting for a new sentinel, then REXX, and the bridge lets R go.
 *   DEAD     P is gone: R's PY.FINAL (the class's UNINIT, once), and the
 *            bridge lets R go for good.
 *
 * Crossings are counted: a sentinel's UNINIT that comes after a crossing it
 * did not see (R was handed to Rexx after the collection that found the
 * sentinel unreachable) only arms a new one.  R is never freed while the
 * bridge knows it: in REXX it is held by Rexx or, between the collection and
 * the drain, by the queue its sentinel put it on; in PYTHON and BACK by the
 * keep Directory.  No ooRexx API call is made with pairsLock held (one may
 * run UNINIT methods, which may need it), nor with the GIL held.
 */
struct Pair
{
    enum State { REXX, PYTHON, BACK, DEAD } state = REXX;
    Inst *inst = NULL;
    RexxObjectPtr r = NULLOBJECT;         // the Rexx object
    PyObject *p = NULL;                   // the Python object
    unsigned long long crossings = 0;     // times P crossed over to Rexx
    unsigned long long armed = 0;         // crossings when the sentinel was made
    unsigned long long sentinel = 0;      // the current sentinel's number
    unsigned long keepKey = 0;            // PYTHON, BACK, DEAD: R in the keep Directory
};
static std::mutex pairsLock;
static std::map<unsigned long long, Pair> pairs;
static unsigned long long pairCounter = 0;
static std::vector<unsigned long long> pairsToArm;     // BACK
static std::vector<unsigned long long> pairsToEnd;     // DEAD
static std::atomic<long> statPairs(0), statPairsEnded(0);

// the pair of a Python object (with the GIL): its id, or 0
static unsigned long long pairId(PyObject *o)
{
    PyObject *id = PyObject_GetAttrString(o, "_oorexx_pair");
    if (id == NULL) { PyErr_Clear(); return 0; }
    unsigned long long n = PyLong_Check(id) ? PyLong_AsUnsignedLongLong(id) : 0;
    Py_DECREF(id);
    if (PyErr_Occurred()) { PyErr_Clear(); return 0; }
    return n;
}

// P crosses over to Rexx (with the GIL): R, which Rexx may hold from now on
static bool pairCross(unsigned long long id, RexxObjectPtr *r, Inst **inst)
{
    std::lock_guard<std::mutex> guard(pairsLock);
    auto it = pairs.find(id);
    if (it == pairs.end() || it->second.state == Pair::DEAD || isClosed(it->second.inst)) return false;
    Pair &e = it->second;
    e.crossings++;
    if (e.state == Pair::PYTHON)
    {
        Py_INCREF(e.p);                   // R holds P again
        e.state = Pair::BACK;
        pairsToArm.push_back(id);
    }
    *r = e.r;
    *inst = e.inst;
    return true;
}

static RexxDirectoryObject keepOf(Inst *inst)
{
    std::lock_guard<std::mutex> guard(instancesLock);
    auto it = instances.find(inst->rexx);
    return it != instances.end() && !isClosed(inst) ? it->second.keep : NULLOBJECT;
}

static unsigned long pin(RexxThreadContext *tc, Inst *inst, RexxObjectPtr r)
{
    RexxDirectoryObject keep = keepOf(inst);
    if (keep == NULLOBJECT) return 0;
    unsigned long key = ++keepCounter;
    char buf[32];
    keyText(key, buf, sizeof buf);
    tc->DirectoryPut(keep, r, buf);
    return key;
}

static void unpin(RexxThreadContext *tc, Inst *inst, unsigned long key)
{
    if (key == 0) return;
    RexxDirectoryObject keep = keepOf(inst);
    if (keep == NULLOBJECT) return;
    char buf[32];
    keyText(key, buf, sizeof buf);
    tc->DirectoryRemove(keep, buf);
}

struct SentinelEvent
{
    RexxObjectPtr r;
    unsigned long long id, n;
};

/*
 * The pairs' events, by the thread that drains the graveyard (a Rexx thread,
 * no GIL, no kernel lock between API calls): the sentinels it found (Rexx no
 * longer holds those R), the pairs that crossed back (BACK) and the pairs
 * that are over (DEAD).
 */
static void drainPairs(RexxThreadContext *tc, const std::vector<SentinelEvent> &events)
{
    std::vector<PyObject *> released;
    for (const SentinelEvent &ev : events)
    {
        Inst *inst;
        bool rearm;
        {
            std::lock_guard<std::mutex> guard(pairsLock);
            auto it = pairs.find(ev.id);
            if (it == pairs.end() || it->second.state != Pair::REXX || it->second.sentinel != ev.n) continue;
            inst = it->second.inst;
            rearm = it->second.crossings != it->second.armed;
        }
        if (!rearm)
        {
            unsigned long key = pin(tc, inst, ev.r);
            {
                std::lock_guard<std::mutex> guard(pairsLock);
                auto it = pairs.find(ev.id);
                if (it != pairs.end() && key != 0 && it->second.state == Pair::REXX &&
                    it->second.sentinel == ev.n && it->second.crossings == it->second.armed)
                {
                    it->second.state = Pair::PYTHON;
                    it->second.keepKey = key;
                    released.push_back(it->second.p);
                    key = 0;
                }
                else rearm = it != pairs.end() && it->second.state == Pair::REXX;
            }
            unpin(tc, inst, key);         // (0 if it is held now)
        }
        if (rearm)
        {
            tc->SendMessage0(ev.r, "PY.ARM");
            if (tc->CheckCondition()) tc->ClearCondition();
        }
    }
    if (!released.empty() && Py_IsInitialized())
    {
        Gil gil;                          // may run P's __del__: DEAD
        for (PyObject *p : released) Py_DECREF(p);
    }

    std::vector<unsigned long long> ids;
    {
        std::lock_guard<std::mutex> guard(pairsLock);
        ids.swap(pairsToArm);
    }
    for (unsigned long long id : ids)
    {
        RexxObjectPtr r;
        Inst *inst;
        unsigned long key;
        {
            std::lock_guard<std::mutex> guard(pairsLock);
            auto it = pairs.find(id);
            if (it == pairs.end() || it->second.state != Pair::BACK) continue;
            it->second.state = Pair::REXX;
            r = it->second.r;
            inst = it->second.inst;
            key = it->second.keepKey;
            it->second.keepKey = 0;
        }
        tc->SendMessage0(r, "PY.ARM");    // a new sentinel, then the bridge lets R go
        if (tc->CheckCondition()) tc->ClearCondition();
        unpin(tc, inst, key);
    }

    ids.clear();
    {
        std::lock_guard<std::mutex> guard(pairsLock);
        ids.swap(pairsToEnd);
    }
    for (unsigned long long id : ids)
    {
        RexxObjectPtr r;
        Inst *inst;
        unsigned long key;
        {
            std::lock_guard<std::mutex> guard(pairsLock);
            auto it = pairs.find(id);
            if (it == pairs.end()) continue;
            r = it->second.r;
            inst = it->second.inst;
            key = it->second.keepKey;
            pairs.erase(it);
        }
        statPairsEnded++;
        if (!isClosed(inst))
        {
            tc->SendMessage0(r, "PY.FINAL");   // the class's UNINIT
            if (tc->CheckCondition()) tc->ClearCondition();
        }
        unpin(tc, inst, key);
    }
}

// oorexx._pair_dead(obj): P's __del__ (with the GIL)
static PyObject *pairDead(PyObject *, PyObject *obj)
{
    unsigned long long id = pairId(obj);
    if (id != 0)
    {
        std::lock_guard<std::mutex> guard(pairsLock);
        auto it = pairs.find(id);
        if (it != pairs.end() && it->second.state == Pair::PYTHON)
        {
            it->second.state = Pair::DEAD;
            pairsToEnd.push_back(id);
        }
    }
    Py_RETURN_NONE;
}

// oorexx._call_peer(obj, rexxName, args, kwargs): Python calls an override:
// R~rexxName(args..., a .PyKwArgs if kwargs)
static PyObject *pairCallPeer(PyObject *, PyObject *a)
{
    PyObject *obj, *name, *args, *kwargs;
    if (!PyArg_UnpackTuple(a, "_call_peer", 4, 4, &obj, &name, &args, &kwargs)) return NULL;
    if (!PyTuple_Check(args)) { PyErr_SetString(PyExc_TypeError, "_call_peer: args must be a tuple"); return NULL; }
    unsigned long long id = pairId(obj);
    RexxObjectPtr r;
    Inst *inst;
    if (id == 0 || !pairCross(id, &r, &inst))
    {
        PyErr_SetString(rexxErrorType, "the Rexx object of this Python object is gone");
        return NULL;
    }
    std::string message = pyString(name);
    bool kw = kwargs != Py_None && PyDict_Check(kwargs) && PyDict_Size(kwargs) > 0;
    PyObject *all = args;
    Py_INCREF(all);
    if (kw)
    {
        PyObject *one = PyTuple_Pack(1, kwargs);
        PyObject *joined = one != NULL ? PySequence_Concat(args, one) : NULL;
        Py_XDECREF(one);
        Py_DECREF(all);
        if (joined == NULL) return NULL;
        all = joined;
    }
    PyObject *result = intoRexx(inst, all, 0, NULL,
        [&](RexxThreadContext *tc, RexxSide &side, RexxArrayObject rargs, std::string &error) -> RexxObjectPtr {
            if (kw)
            {
                size_t n = tc->ArraySize(rargs);
                if (side.kwClass == NULLOBJECT) { error = "python.cls is not loaded"; return NULLOBJECT; }
                RexxObjectPtr k = tc->SendMessage1(side.kwClass, "NEW", tc->ArrayOfOne(tc->ArrayAt(rargs, n)));
                if (k == NULLOBJECT) return NULLOBJECT;
                tc->ArrayPut(rargs, k, n);
            }
            return tc->SendMessage(r, message.c_str(), rargs);
        });
    Py_DECREF(all);
    return result;
}

static PyMethodDef pairFunctions[] =
{
    {"_call_peer", (PyCFunction)pairCallPeer, METH_VARARGS, "for python.cls: Python calls an override"},
    {"_pair_dead", (PyCFunction)pairDead, METH_O, "for python.cls: a pair's Python object is gone"},
    {NULL, NULL, 0, NULL}
};

// oorexx._Pair: a base of every Python class that a Rexx class extends (no
// state: any base can take it as a mixin)
static PyObject *makePairType()
{
    PyObject *slots = PyTuple_New(0);
    PyObject *bases = PyTuple_Pack(1, (PyObject *)&PyBaseObject_Type);
    PyObject *dict = PyDict_New();
    PyObject *type = NULL;
    PyObject *module = PyUnicode_FromString("oorexx");
    if (slots != NULL && bases != NULL && dict != NULL && module != NULL &&
        PyDict_SetItemString(dict, "__slots__", slots) == 0 &&
        PyDict_SetItemString(dict, "__module__", module) == 0)
        type = PyObject_CallFunction((PyObject *)&PyType_Type, "sOO", "_Pair", bases, dict);
    Py_XDECREF(module);
    Py_XDECREF(slots);
    Py_XDECREF(bases);
    Py_XDECREF(dict);
    return type;
}

// interpreter closed: its pairs are over (their Rexx objects go with it)
static void forgetPairs(Inst *h)
{
    std::lock_guard<std::mutex> guard(pairsLock);
    for (auto it = pairs.begin(); it != pairs.end(); )
    {
        if (it->second.inst == h) it = pairs.erase(it);
        else ++it;
    }
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

// Python's keywords (the hard ones, 3.9 on: they cannot be attribute names)
static bool isPythonKeyword(const std::string &s)
{
    static const char *keywords[] =
    {
        "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class",
        "continue", "def", "del", "elif", "else", "except", "finally", "for", "from", "global",
        "if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise",
        "return", "try", "while", "with", "yield", NULL
    };
    for (const char **k = keywords; *k != NULL; k++) if (s == *k) return true;
    return false;
}

static PyObject *rexxProxyGetAttr(PyObject *self, PyObject *name)
{
    // Python's own protocol names are not Rexx messages (numpy and friends
    // probe __array__, __len__... on any object)
    std::string s = pyString(name);
    if (s.size() >= 2 && s[0] == '_' && s[1] == '_') return PyObject_GenericGetAttr(self, name);
    if (isSendName(s)) return PyObject_GenericGetAttr(self, name);   // sendMessage...: ours
    // a Rexx message named like a Python keyword cannot be written as an
    // attribute (ctx.raise is a syntax error): PEP 8's trailing underscore
    // stands for it, ctx.raise_ sends RAISE (one underscore, after a keyword)
    if (s.size() > 1 && s.back() == '_' && isPythonKeyword(s.substr(0, s.size() - 1))) s.pop_back();
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

// == and hash: the same Rexx object (as .PyObject's == is Python's is), so
// two proxies of one object are equal and can be the same dict key.  ooRexx
// does not move live objects, and the proxy keeps its object alive.
static PyObject *rexxProxyRichCompare(PyObject *a, PyObject *b, int op)
{
    Inst *ia, *ib;
    RexxObjectPtr ta, tb;
    if ((op != Py_EQ && op != Py_NE) || !rexxProxyTarget(a, &ia, &ta) || !rexxProxyTarget(b, &ib, &tb))
        Py_RETURN_NOTIMPLEMENTED;
    bool same = ta == tb;
    if ((op == Py_EQ) == same) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static Py_hash_t rexxProxyHash(PyObject *self)
{
    Py_hash_t h = (Py_hash_t)((uintptr_t)((RexxProxy *)self)->target >> 4);
    return h == -1 ? -2 : h;
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
    {Py_tp_richcompare, (void *)rexxProxyRichCompare},
    {Py_tp_hash, (void *)rexxProxyHash},
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

extern PyType_Spec interpreterSpec;
static PyObject *interpreterType = NULL;

// the types, made once (with the GIL)
static bool makeTypes()
{
    if (rexxProxyType != NULL) return true;
    rexxProxyType = PyType_FromSpec(&rexxProxySpec);
    rexxBoundType = PyType_FromSpec(&rexxBoundSpec);
    // a Rexx condition's parts as attributes (raiseRexxError); None when the
    // error is no Rexx condition (a closed instance...)
    PyObject *attrs = PyDict_New();
    if (attrs != NULL)
    {
        for (const char *a : {"condition", "code", "rc", "message", "errortext", "position",
                              "program", "description", "traceback"})
        {
            PyDict_SetItemString(attrs, a, Py_None);
        }
        PyObject *doc = PyUnicode_FromString(
            "A Rexx condition in Rexx code that Python called: condition, code (SYNTAX's, \"42.3\"), "
            "rc, message, errortext, position (the line), program, description, traceback (the Rexx "
            "traceback, as Rexx shows it).");
        if (doc != NULL) { PyDict_SetItemString(attrs, "__doc__", doc); Py_DECREF(doc); }
    }
    rexxErrorType = PyErr_NewException("oorexx.RexxError", PyExc_RuntimeError, attrs);
    Py_XDECREF(attrs);
    interpreterType = PyType_FromSpec(&interpreterSpec);
    return rexxProxyType != NULL && rexxBoundType != NULL && rexxErrorType != NULL &&
           interpreterType != NULL;
}

// the module's contents (also when Rexx started Python: then the module is
// put in sys.modules, so Python code can "import oorexx")
static bool fillModule(PyObject *m)
{
    if (!makeTypes()) return false;
    PyObject_SetAttrString(m, "RexxObject", rexxProxyType);
    PyObject_SetAttrString(m, "RexxError", rexxErrorType);
    PyObject_SetAttrString(m, "Interpreter", interpreterType);
    if (pairType == NULL) pairType = makePairType();
    if (pairType == NULL) return false;
    PyObject_SetAttrString(m, "_Pair", pairType);
    for (PyMethodDef *d = pairFunctions; d->ml_name != NULL; d++)
    {
        PyObject *f = PyCFunction_NewEx(d, NULL, NULL);
        if (f == NULL) return false;
        PyObject_SetAttrString(m, d->ml_name, f);
        Py_DECREF(f);
    }
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
 *                          stdio=True, prefixes=True)
 *                                      (docs/design.md, *Interpreter options*)
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
    // through a symbolic link (an installation links the loader into ooRexx's
    // lib directory, python.cls into its bin), the directory of the real file
    char *real = realpath(info.dli_fname, NULL);
    std::string path = real != NULL ? real : info.dli_fname;
    free(real);
    size_t slash = path.rfind('/');
#endif
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

/*
 * ooRexx's own .cls folder (logicalwrapper.cls, json.cls...), which every
 * interpreter we create searches: python.cls requires logicalwrapper.cls,
 * and ::requires looks in the current folder, REXX_PATH and the PATH, where
 * ooRexx's folder may not be when Python starts it.  The folder of rexx.dll
 * (Windows); the bin/ beside the lib/ of librexx (Linux, macOS: the packages
 * and the portable builds), or librexx's own folder.  "" if none of them has
 * logicalwrapper.cls.
 */
static std::string rexxClassDirectory()
{
    std::string dir;
#ifdef _WIN32
    HMODULE rexx = GetModuleHandleA("rexx.dll");
    char buf[MAX_PATH];
    if (rexx == NULL || GetModuleFileNameA(rexx, buf, sizeof buf) == 0) return "";
    dir = buf;
    size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos) return "";
    dir.resize(slash);
    std::vector<std::string> candidates = {dir};
    const char *sep = "\\";
#else
    Dl_info info;
    if (dladdr((void *)&RexxCreateInterpreter, &info) == 0 || info.dli_fname == NULL) return "";
    char *real = realpath(info.dli_fname, NULL);
    dir = real != NULL ? real : info.dli_fname;
    free(real);
    size_t slash = dir.rfind('/');
    if (slash == std::string::npos) return "";
    dir.resize(slash);
    slash = dir.rfind('/');
    std::vector<std::string> candidates;
    if (slash != std::string::npos) candidates.push_back(dir.substr(0, slash) + "/bin");
    candidates.push_back(dir);
    const char *sep = "/";
#endif
    for (const std::string &c : candidates)
    {
        std::string f = c + sep + "logicalwrapper.cls";
#ifdef _WIN32
        if (GetFileAttributesA(f.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
#else
        if (access(f.c_str(), R_OK) == 0) return c;
#endif
    }
    return "";
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

static PyObject *registerHandler(Inst *inst, PyObject *nameObj, PyObject *handler);
static PyObject *interpreterClose(PyObject *self, PyObject *);

static PyObject *interpreterNew(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static const char *keywords[] = {"rexx_path", "extensions", "initial_address", "load_libraries",
                                     "command_environments", "stdio", "prefixes", NULL};
    PyObject *rexxPath = NULL, *extensions = NULL, *initialAddress = NULL, *libraries = NULL;
    PyObject *environments = NULL, *prefixesObj = NULL;
    int stdio = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OOOOOpO:Interpreter", (char **)keywords,
                                     &rexxPath, &extensions, &initialAddress, &libraries,
                                     &environments, &stdio, &prefixesObj))
    {
        return NULL;
    }
    // prefixes: True (BSF4ooRexx's), False, or a dict stream -> str or None
    bool prefixes = true;
    std::vector<std::pair<std::string, std::string>> prefixStrings;
    std::vector<bool> prefixNone;                                 // None: no prefix
    bool prefixDict = prefixesObj != NULL && PyDict_Check(prefixesObj);
    if (prefixDict)
    {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(prefixesObj, &pos, &key, &value))
        {
            if (!PyUnicode_Check(key) || (value != Py_None && !PyUnicode_Check(value)))
            {
                PyErr_SetString(PyExc_TypeError, "prefixes: a dict stream name -> str or None");
                return NULL;
            }
            prefixStrings.push_back({pyString(key), value == Py_None ? "" : pyString(value)});
            prefixNone.push_back(value == Py_None);
        }
    }
    else if (prefixesObj != NULL)
    {
        int t = PyObject_IsTrue(prefixesObj);
        if (t < 0) return NULL;
        prefixes = t != 0;
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
    std::string classes = rexxClassDirectory();      // ooRexx's own .cls files
    if (!classes.empty() && classes != dir) path.push_back(classes);
#ifdef _WIN32
    std::string pathText = join(path, ";");
#else
    std::string pathText = join(path, ":");
#endif
    std::string extText = join(exts, ",");
    std::string address = initialAddress != NULL && initialAddress != Py_None ? pyString(initialAddress) : "";

    static RexxLibraryPackage library[] = {{"rexxpython", orxpy_package()}, {NULL, NULL}};
    std::vector<RexxOption> options;
    RexxOption o;
    o.optionName = EXTERNAL_CALL_PATH; o.option = pathText.c_str(); options.push_back(o);
    if (!exts.empty()) { o.optionName = EXTERNAL_CALL_EXTENSIONS; o.option = extText.c_str(); options.push_back(o); }
    if (!address.empty()) { o.optionName = INITIAL_ADDRESS_ENVIRONMENT; o.option = address.c_str(); options.push_back(o); }
    for (const std::string &l : libs) { o.optionName = LOAD_REQUIRED_LIBRARY; o.option = l.c_str(); options.push_back(o); }
    o.optionName = REGISTER_LIBRARY; o.option = (POINTER)library; options.push_back(o);
    o.optionName = NULL; options.push_back(o);

    Interpreter *self = (Interpreter *)((allocfunc)PyType_GetSlot(type, Py_tp_alloc))(type, 0);
    if (self == NULL) return NULL;

    std::string error;
    RexxInstance *inst = NULL;
    PyThreadState *saved = PyEval_SaveThread();
    RexxThreadContext *tc;
#ifndef _WIN32
    // Python is the host: it keeps its signals (*Signals*)
    std::unique_lock<std::mutex> signalsGuard(signalsLock, std::defer_lock);
    if (!pythonOwned) signalsGuard.lock();
    HostSignals hostSignals;
#endif
    bool created = RexxCreateInterpreter(&inst, &tc, options.data());
#ifndef _WIN32
    if (!pythonOwned)
    {
        hostSignals.restore();
        if (created) haltableAdd(inst);
        signalsGuard.unlock();
    }
#endif
    if (!created)
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
            setBooleans(tc, d, tc->FindPackageClass(package, "LOGICALWRAPPER"));
            RexxClassObject coreHandler = tc->FindClass("COMMANDHANDLER");
            RexxClassObject pyHandler = tc->FindPackageClass(package, "PYCOMMANDHANDLER");
            if (coreHandler != NULLOBJECT && pyHandler != NULLOBJECT)
            {
                d.commandHandlerClass = (RexxClassObject)tc->RequestGlobalReference(coreHandler);
                d.pyCommandHandlerClass = (RexxClassObject)tc->RequestGlobalReference(pyHandler);
            }
            RexxObjectPtr closing = tc->DirectoryAt(tc->GetPackageRoutines(package), "PY.CLOSING");
            if (closing != NULLOBJECT) d.closing = (RexxRoutineObject)tc->RequestGlobalReference(closing);
            tc->ClearCondition();
            {
                std::lock_guard<std::mutex> guard(instancesLock);
                instances[inst] = d;
            }
            // standard streams through Python's: py.redirectTo (python.cls)
            if (stdio)
            {
                RexxObjectPtr prefixArg = prefixes ? tc->True() : tc->False();
                if (prefixDict)
                {
                    RexxStringTableObject table = tc->NewStringTable();
                    for (size_t i = 0; i < prefixStrings.size(); i++)
                    {
                        RexxObjectPtr v = prefixNone[i] ? tc->Nil() : (RexxObjectPtr)tc->String(prefixStrings[i].second.c_str(), prefixStrings[i].second.size());
                        tc->StringTablePut(table, v, prefixStrings[i].first.c_str());
                    }
                    prefixArg = table;
                }
                RexxArrayObject a = tc->NewArray(7);
                tc->ArrayPut(a, tc->String("P"), 1);
                tc->ArrayPut(a, prefixArg, 7);
                RexxRoutineObject redirect = (RexxRoutineObject)tc->DirectoryAt(tc->GetPackageRoutines(package), "PY.REDIRECTTO");
                if (redirect != NULLOBJECT) tc->CallRoutine(redirect, a);
                if (redirect == NULLOBJECT || tc->CheckCondition())
                {
                    error = tc->CheckCondition() ? takeCondition(tc) : "python.cls: py.redirectTo not found";
                }
            }
        }
        // the creating thread stays attached (its root activity): detaching
        // it makes Terminate wait forever (ooRexx bug #2107, still there in
        // r13271; notes/oorexx-bugs.md)
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
            if (!PyUnicode_Check(key) || !PyCallable_Check(value))
            {
                PyErr_SetString(PyExc_TypeError, "command_environments: a dict name -> callable");
                interpreterClose((PyObject *)self, NULL);
                Py_DECREF(self);
                return NULL;
            }
            PyObject *covered = registerHandler(self->inst, key, value);
            if (covered == NULL)
            {
                interpreterClose((PyObject *)self, NULL);
                Py_DECREF(self);
                return NULL;
            }
            Py_DECREF(covered);
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
        RexxRoutineObject closing = NULLOBJECT;
        RexxObjectPtr graveyard = NULLOBJECT;
        {
            std::lock_guard<std::mutex> guard(instancesLock);
            auto it = instances.find(h->rexx);
            if (it != instances.end()) { keep = it->second.keep; closing = it->second.closing; graveyard = it->second.graveyard; }
        }
        forgetInstance(h);
        forgetPairs(h);
        Py_BEGIN_ALLOW_THREADS
        // the Rexx objects Python still holds through this instance's proxies
        // are let go (the proxies are unusable from now on); those another
        // instance still reaches survive, since all instances share one object
        // memory (docs/design.md, *Lifetime*)
        RexxThreadContext *tc;
        if (keep != NULLOBJECT && h->rexx->AttachThread(&tc))
        {
            tc->SendMessage0(keep, "EMPTY");
            // its ADDRESS environments written in Python let their callables
            // go now: a collection that would run their uninits may never come
            // while other instances live (docs/design.md, *Command environments*)
            if (closing != NULLOBJECT)
            {
                tc->CallRoutine(closing, tc->NewArray(0));
                tc->ClearCondition();
                drainGraveyard(tc, graveyard);
            }
            tc->DetachThread();
        }
#ifndef _WIN32
        haltableRemove(h->rexx);
#endif
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
static PyObject *interpreterRemoveEnvironment(PyObject *self, PyObject *args);

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
    {"remove_command_environment", (PyCFunction)interpreterRemoveEnvironment, METH_VARARGS,
     "remove_command_environment(name): ADDRESS name is unknown again (FAILURE, RC 30); ValueError if it was not registered"},
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
/* ADDRESS environments written in Python, as ooRexx's own (RFE #882)         */
/* ------------------------------------------------------------------------ */

/*
 * A Python callable is a command handler like any other: python.cls's
 * .PyCommandHandler (it inherits .CommandHandler) calls it as
 * handler(command, context), context being ooRexx's own .CommandContext, and
 * its result is RC (None: 0).  From Rexx it is registered with
 * .CommandHandler~register(name, .py~commandHandler(handler)); from Python,
 *
 *   r.add_command_environment("CALC", calc)
 *
 * is that same register, and remove_command_environment(name) the core's
 * deregister: stacking, the results and the errors are ooRexx's.  So the
 * bridge has no native command handler of its own (docs/design.md, *Command
 * environments*).
 */

// name served by handler, in the instance (the Python side: with the GIL)
static PyObject *registerHandler(Inst *inst, PyObject *nameObj, PyObject *handler)
{
    PyObject *args = PyTuple_Pack(2, nameObj, handler);
    if (args == NULL) return NULL;
    PyObject *r = intoRexx(inst, args, 0, NULL,
        [&](RexxThreadContext *tc, RexxSide &, RexxArrayObject rargs, std::string &error) -> RexxObjectPtr {
            RexxClassObject core = NULLOBJECT, ours = NULLOBJECT;
            {
                std::lock_guard<std::mutex> guard(instancesLock);
                auto it = instances.find(tc->instance);
                if (it != instances.end()) { core = it->second.commandHandlerClass; ours = it->second.pyCommandHandlerClass; }
            }
            if (core == NULLOBJECT || ours == NULLOBJECT)
            {
                error = "add_command_environment: this ooRexx has no .CommandHandler (5.3.0 r13289 or later is needed)";
                return NULLOBJECT;
            }
            RexxObjectPtr h = tc->SendMessage1(ours, "NEW", tc->ArrayAt(rargs, 2));
            if (h == NULLOBJECT || tc->CheckCondition()) return NULLOBJECT;
            return tc->SendMessage2(core, "REGISTER", tc->ArrayAt(rargs, 1), h);
        });
    Py_DECREF(args);
    return r;
}

// r.add_command_environment(name, handler): the handler it covers, or None
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
    return registerHandler(r->inst, nameObj, handler);
}

// r.remove_command_environment(name): the handler removed
static PyObject *interpreterRemoveEnvironment(PyObject *self, PyObject *args)
{
    Interpreter *r = (Interpreter *)self;
    if (!liveInterpreter(r)) return NULL;
    PyObject *nameObj;
    if (!PyArg_ParseTuple(args, "U:remove_command_environment", &nameObj)) return NULL;
    return intoRexx(r->inst, args, 0, NULL,
        [&](RexxThreadContext *tc, RexxSide &, RexxArrayObject rargs, std::string &error) -> RexxObjectPtr {
            RexxClassObject core = NULLOBJECT;
            {
                std::lock_guard<std::mutex> guard(instancesLock);
                auto it = instances.find(tc->instance);
                if (it != instances.end()) core = it->second.commandHandlerClass;
            }
            if (core == NULLOBJECT)
            {
                error = "remove_command_environment: this ooRexx has no .CommandHandler (5.3.0 r13289 or later is needed)";
                return NULLOBJECT;
            }
            return tc->SendMessage1(core, "DEREGISTER", tc->ArrayAt(rargs, 1));
        });
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

// a .PyObject whose Python object is not there: a pair whose INIT did not
// call init:super, or whose Python object is gone (in its class's UNINIT)
static RexxObjectPtr noPython(RexxMethodContext *context)
{
    context->RaiseException1(Rexx_Error_Execution_user_defined,
        context->String("ooRexx/Python: this object has no Python object (its INIT must call "
                        "self~init:super(...); in its class's UNINIT, the Python object is gone)"));
    return NULLOBJECT;
}

/*
 * ~"PY.PAIRNEW"(pyclass, args): a pair's Python object, made by the base's
 * __new__ with args (python.cls, PyObject's INIT: self~init:super(args));
 * answers (pointer, pair id).  See *Pairs*.
 */
RexxMethod3(RexxObjectPtr, PyPair_New, OSELF, self, RexxObjectPtr, pyclass, RexxArrayObject, rexxArgs_)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
    if (!instanceOf(context->threadContext, pyclass, side.pyObjectClass)) return noPython(context);
    RexxObjectPtr cp = context->SendMessage0(pyclass, "PY.POINTER");
    PyObject *cls = cp != NULLOBJECT ? (PyObject *)context->PointerValue((RexxPointerObject)cp) : NULL;
    if (cls == NULL) return noPython(context);
    Args args;
    rexxArgs(side, rexxArgs_, 1, args);
    unsigned long long id;
    {
        std::lock_guard<std::mutex> guard(pairsLock);
        id = ++pairCounter;
        Pair &e = pairs[id];
        e.inst = handleOf(side.inst);
        e.r = self;
    }
    statPairs++;
    PyError err;
    PyObject *obj = NULL;
    {
        Gil gil;
        PyObject *tuple = NULL, *dict = NULL;
        if (!buildArgs(args, &tuple, &dict)) captureError(err);
        else
        {
            obj = PyObject_CallMethod(cls, "_oorexx_new", "KOO", id, tuple, dict != NULL ? dict : Py_None);
            if (obj == NULL) captureError(err);
            else
            {
                std::lock_guard<std::mutex> guard(pairsLock);
                pairs[id].p = obj;        // R's reference (its CSELF)
            }
        }
        Py_XDECREF(tuple);
        Py_XDECREF(dict);
    }
    if (err.set)
    {
        {
            std::lock_guard<std::mutex> guard(pairsLock);
            pairs.erase(id);
        }
        Val none;
        return finish(side, none, err);
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%llu", id);
    RexxArrayObject made = context->NewArray(2);
    context->ArrayPut(made, context->NewPointer(obj), 1);
    context->ArrayPut(made, context->String(buf), 2);
    return made;
}

// ~"PY.PAIRINIT"(args): the base's __init__ with args, once the Rexx object
// has its Python object (an __init__ may call overrides)
RexxMethod2(RexxObjectPtr, PyPair_Init, RexxArrayObject, rexxArgs_, CSELF, self)
{
    if (self == NULL) return noPython(context);
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
    Args args;
    rexxArgs(side, rexxArgs_, 1, args);
    PyError err;
    Val none;
    {
        Gil gil;
        PyObject *obj = selfObject(self);
        PyObject *tuple = NULL, *dict = NULL;
        if (!buildArgs(args, &tuple, &dict)) captureError(err);
        else
        {
            PyObject *r = PyObject_CallMethod((PyObject *)Py_TYPE(obj), "_oorexx_init", "OOO",
                                              obj, tuple, dict != NULL ? dict : Py_None);
            if (r == NULL) captureError(err);
            Py_XDECREF(r);
        }
        Py_XDECREF(tuple);
        Py_XDECREF(dict);
    }
    if (err.set) return finish(side, none, err);
    return NULLOBJECT;
}

// PyPair_Arm(id): a new sentinel's number (Rexx holds the pair's Rexx object)
RexxRoutine1(RexxObjectPtr, PyPair_Arm, CSTRING, idText)
{
    unsigned long long id = strtoull(idText, NULL, 10), n = 0;
    {
        std::lock_guard<std::mutex> guard(pairsLock);
        auto it = pairs.find(id);
        if (it != pairs.end())
        {
            n = ++it->second.sentinel;
            it->second.armed = it->second.crossings;
        }
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%llu", n);
    return context->String(buf);
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
    std::vector<SentinelEvent> sentinels;      // (R, pair, n): see *Pairs*
    for (;;)
    {
        RexxObjectPtr p = tc->SendMessage0(graveyard, "PULL");
        if (p == NULLOBJECT || p == tc->Nil()) break;
        if (tc->IsArray(p))
        {
            RexxArrayObject a = (RexxArrayObject)p;
            SentinelEvent ev;
            ev.r = tc->ArrayAt(a, 1);
            ev.id = strtoull(tc->ObjectToStringValue(tc->ArrayAt(a, 2)), NULL, 10);
            ev.n = strtoull(tc->ObjectToStringValue(tc->ArrayAt(a, 3)), NULL, 10);
            if (ev.r != NULLOBJECT) sentinels.push_back(ev);
            continue;
        }
        void *v = tc->PointerValue((RexxPointerObject)p);
        if (v != NULL) dead.push_back((PyObject *)v);
    }
    if (!dead.empty() && Py_IsInitialized())
    {
        Gil gil;
        for (PyObject *o : dead) Py_DECREF(o);
    }
    drainPairs(tc, sentinels);
}

// super(cls, o) if name is one that o's class (made by py.extend) overrides,
// with its Python name in exact; else NULL (with the GIL; no error set)
static PyObject *pairSuper(PyObject *o, const std::string &name, std::string &exact)
{
    PyObject *type = (PyObject *)Py_TYPE(o);
    PyObject *overrides = PyObject_GetAttrString(type, "_oorexx_overrides");
    if (overrides == NULL) { PyErr_Clear(); return NULL; }
    PyObject *py = PyDict_Check(overrides) ? PyDict_GetItemString(overrides, name.c_str()) : NULL;
    PyObject *base = NULL;
    if (py != NULL)
    {
        exact = pyString(py);
        PyObject *cls = PyObject_GetAttrString(type, "_oorexx_class");
        if (cls != NULL) base = PyObject_CallFunctionObjArgs((PyObject *)&PySuper_Type, cls, o, NULL);
        Py_XDECREF(cls);
        if (base == NULL) PyErr_Clear();
    }
    Py_DECREF(overrides);
    return base;
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
    if (self == NULL) return noPython(context);
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
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
        bool found;
        // a pair's Rexx object: a name its class overrides is the base's
        // member (self~name:super(args) reaches here), through super()
        PyObject *base = pairType != NULL && PyObject_TypeCheck(o, (PyTypeObject *)pairType)
                         ? pairSuper(o, name, exact) : NULL;
        if (base != NULL)
        {
            o = base;                     // (dropped below)
            found = true;
        }
        else found = resolveName(o, name, &exact, err);
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
        Py_XDECREF(base);
    }
    return finish(side, result, err);
}

// ~"PY.HAS"(rexxName): does the name resolve (docs/design.md, *Names*) to an attribute?
RexxMethod2(logical_t, PyObj_Has, CSTRING, name, CSELF, self)
{
    if (self == NULL) return 0;
    Gil gil;
    std::string exact;
    PyError err;
    return resolveName(selfObject(self), name, &exact, err);
}

// ~getattr(exactName)
RexxMethod2(RexxObjectPtr, PyObj_GetAttr, CSTRING, name, CSELF, self)
{
    if (self == NULL) return noPython(context);
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
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
    if (self == NULL) return noPython(context);
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
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
    if (self == NULL) return noPython(context);
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
    RexxObjectPtr target = context->ArraySize(all) >= 1 ? context->ArrayAt(all, 1) : NULLOBJECT;
    bool callSelf = target == NULLOBJECT || target == context->Nil();
    std::string name = callSelf ? "" : textOf(context->threadContext, target);
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

// the key of o[k]: positions and keys follow the rule even through a module
// imported as "string" (l[0] on a list it returned)
static PyObject *plainKey(const Args &keys)
{
    StringsScope plain(false);
    return itemKey(keys);
}

// o[keys...]
RexxMethod2(RexxObjectPtr, PyObj_GetItem, ARGLIST, all, CSELF, self)
{
    if (self == NULL) return noPython(context);
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
    Args keys;
    rexxArgs(side, all, 1, keys);
    Val result;
    PyError err;
    {
        Gil gil;
        PyObject *key = plainKey(keys);
        PyObject *r = key != NULL ? PyObject_GetItem(selfObject(self), key) : NULL;
        Py_XDECREF(key);
        if (r == NULL) captureError(err); else pyToVal(r, result);
    }
    return finish(side, result, err);
}

// o[keys...] = value  (Rexx passes the value first)
RexxMethod2(RexxObjectPtr, PyObj_SetItem, ARGLIST, all, CSELF, self)
{
    if (self == NULL) return noPython(context);
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
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
        PyObject *key = plainKey(keys);
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
    if (self == NULL) return noPython(context);
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
    if (self == NULL) return noPython(context);
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
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
    if (self == NULL) return noPython(context);
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
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
    if (self == NULL) return noPython(context);
    char buf[32];
    snprintf(buf, sizeof buf, "%p", self);
    return context->String(buf);
}

/* ------------------------------------------------------------------------ */
/* .py (a .PyObject for builtins, plus the bridge's own helpers)              */
/* ------------------------------------------------------------------------ */

// .py~import(name): the module, always as a proxy
RexxMethod2(RexxObjectPtr, Py_Import, CSTRING, name, OPTIONAL_CSTRING, type)
{
    RexxSide side;
    if (!findClasses(context, side)) return NULLOBJECT;
    StringsScope scope(side.strings);
    if (type != NULL)
    {
        std::string t = lower(type);
        if (t == "string") side.strings = true;     // the module, and all it reaches
        else if (t != "number")
        {
            context->RaiseException1(Rexx_Error_Incorrect_method_user_defined,
                context->String(("py.import(name, type): type must be \"number\" or \"string\", not \"" +
                                 std::string(type) + "\"").c_str()));
            return NULLOBJECT;
        }
    }
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
    StringsScope scope(side.strings);
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
                // .py~float of a Rexx string: in range, or an error
                if (kind == "float" && !err.set && args.positional.size() == 1 && args.keywords.empty() &&
                    args.positional[0].kind == Val::STR && result.kind == Val::REF &&
                    !floatInRange(result.ref, args.positional[0].s))
                {
                    captureError(err);
                    dropVal(result);
                    result.kind = Val::NIL;
                }
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
    StringsScope scope(side.strings);
    std::string text = textOf(context->threadContext, value);
    std::string kind = "str";
    Args holder;                          // a Python callable given as the type
    if (type != NULLOBJECT)
    {
        if (instanceOf(context->threadContext, type, side.pyObjectClass))
        {
            kind = "";
            holder.positional.emplace_back();
            rexxToVal(side, type, holder.positional[0]);
        }
        else kind = lower(textOf(context->threadContext, type));
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
            if (r != NULL && (kind == "float" || kind == "complex") && !floatInRange(r, text))
                Py_CLEAR(r);
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
        fprintf(stderr, "orxpy: pairs %ld, over %ld\n", statPairs.load(), statPairsEnded.load());
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
    REXX_METHOD(PyPair_New,      PyPair_New),
    REXX_METHOD(PyPair_Init,     PyPair_Init),
    REXX_LAST_METHOD()
};

static RexxRoutineEntry bridgeRoutines[] =
{
    REXX_TYPED_ROUTINE(PyPair_Arm, PyPair_Arm),
    REXX_LAST_ROUTINE()
};

static RexxPackageEntry bridgePackage =
{
    STANDARD_PACKAGE_HEADER
    REXX_INTERPRETER_5_0_0,
    "rexxpython",
    "0.1",
    packageLoad,
    packageUnload,
    bridgeRoutines,
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
 * ooRexx's bin directory is.  Before the first call into it, find it
 * ourselves (the bridge's directory, OOREXX_HOME, the PATH): once loaded, the
 * delay-load helper finds it by name.
 */
static void rexxFolderOnPath();

static bool preloadRexx()
{
    if (GetModuleHandleA("rexx.dll") != NULL) { rexxFolderOnPath(); return true; }
    // first the bridge's own directory: installed (install.rex), it sits in
    // ooRexx's own, next to the rexx.dll it belongs with
    const char *home = getenv("OOREXX_HOME");
    std::string dirs = bridgeDirectory() + ";";
    if (home != NULL) dirs += std::string(home) + "\\bin;";
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
        if (LoadLibraryExA(dll.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH) != NULL)
        {
            rexxFolderOnPath();
            return true;
        }
    }
    PyErr_SetString(PyExc_ImportError,
        "oorexx: rexx.dll not found: put ooRexx's bin directory on the PATH (or set OOREXX_HOME)");
    return false;
}

/*
 * ooRexx ends the process (0x80000003, no message) in RexxCreateInterpreter
 * when rexx.dll was loaded by its full path and its folder is not on the
 * PATH (an ooRexx bug; found by the ooRexx bridge for .NET).  So, once
 * loaded, rexx.dll's folder goes at the end of this process's PATH if it is
 * not there.
 */
static void rexxFolderOnPath()
{
    HMODULE rexx = GetModuleHandleA("rexx.dll");
    char buf[MAX_PATH];
    if (rexx == NULL || GetModuleFileNameA(rexx, buf, sizeof buf) == 0) return;
    std::string dir = buf;
    size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos) return;
    dir.resize(slash);
    std::string path;
    DWORD n = GetEnvironmentVariableA("PATH", NULL, 0);
    if (n > 0)
    {
        std::vector<char> v(n);
        GetEnvironmentVariableA("PATH", v.data(), n);
        path = v.data();
    }
    size_t start = 0;
    while (start <= path.size())
    {
        size_t end = path.find(';', start);
        if (end == std::string::npos) end = path.size();
        std::string entry = path.substr(start, end - start);
        while (!entry.empty() && (entry.back() == '\\' || entry.back() == '/')) entry.pop_back();
        if (_stricmp(entry.c_str(), dir.c_str()) == 0) return;   // there already
        start = end + 1;
    }
    path = path.empty() ? dir : path + ";" + dir;
    SetEnvironmentVariableA("PATH", path.c_str());   // what ooRexx reads (the process's)
    _putenv_s("PATH", path.c_str());                 // and the C runtime's copy
}
#endif

PyMODINIT_FUNC PyInit_oorexx(void)
{
#ifdef _WIN32
    if (!preloadRexx()) return NULL;
    // Python is the host: it keeps Ctrl-C and Ctrl-Break (*Signals*)
    if (!pythonOwned) SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#endif
    PyObject *m = PyModule_Create(&moduleDef);
    if (m == NULL) return NULL;
    if (!fillModule(m)) { Py_DECREF(m); return NULL; }
    return m;
}
