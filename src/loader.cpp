/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * loader.cpp: librexxpython.so (librexxpython.dylib on macOS, rexxpython.dll
 * on Windows), the ooRexx native package of the ooRexx/Python bridge
 * (docs/design.md, *Two libraries*).  It holds no bridge code: it makes sure a
 * Python is in the process and then hands ooRexx the package of the real
 * bridge, oorexx.abi3.so (oorexx.pyd on Windows: the same file Python imports
 * as the module "oorexx").
 *
 *  - If Python is already in the process (a Python program running Rexx code
 *    that requires python.cls), it is used: nothing else is loaded.
 *  - Otherwise the Python to use is found (ORX_PYTHON, else "python3" on the
 *    PATH; "python" on Windows), asked for its library, home and sys.path,
 *    and its library is loaded: on Linux and macOS with RTLD_GLOBAL, so that
 *    C extension modules (numpy...) find the Py* symbols; on Windows
 *    python3X.dll and python3.dll from its home, which is where the bridge's
 *    and the extension modules' imports of them are then found.  Only then is
 *    the bridge loaded: its Py* references can be bound.  The bridge
 *    initializes Python when ooRexx loads the package.
 *
 * Platform-dependent by nature (docs/design.md, *Platforms*).
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "oorexxapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#define LOADER_EXPORT __declspec(dllexport)
#define popen _popen
#define pclose _pclose
static const char *defaultPython = "python";
static const char *bridgeFile = "oorexx.pyd";
#else
#include <dlfcn.h>
#define LOADER_EXPORT __attribute__((visibility("default")))
static const char *defaultPython = "python3";
static const char *bridgeFile = "oorexx.abi3.so";
#endif

// what the bridge exports for us (bridge.cpp)
typedef RexxPackageEntry *(*BridgePackageFn)(void);
typedef void (*BridgeLaunchFn)(const char *home, const char *executable,
                               const char *const *path, int npath);

static void complain(const std::string &what)
{
    fprintf(stderr, "ooRexx/Python: %s\n", what.c_str());
}

// the directory this library was loaded from (the bridge sits next to it)
static std::string ownDirectory()
{
#ifdef _WIN32
    HMODULE self;
    char buf[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&ownDirectory, &self) ||
        GetModuleFileNameA(self, buf, sizeof buf) == 0)
    {
        return ".";
    }
    std::string path = buf;
    size_t slash = path.find_last_of("\\/");
#else
    Dl_info info;
    if (dladdr((void *)&ownDirectory, &info) == 0 || info.dli_fname == NULL)
    {
        return ".";
    }
    std::string path = info.dli_fname;
    size_t slash = path.rfind('/');
#endif
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// "exe" -c "code" as a command line for popen (code has no double quotes)
static std::string commandLine(const std::string &exe, const std::string &code)
{
#ifdef _WIN32
    // cmd /c strips the outer pair of quotes when the line starts with one
    return "\"\"" + exe + "\" -c \"" + code + "\" 2>nul\"";
#else
    std::string q = "'";
    for (char c : exe)
    {
        if (c == '\'') q += "'\\''"; else q += c;
    }
    return q + "' -c \"" + code + "\" 2>/dev/null";
#endif
}

// what the chosen Python executable says about itself
struct PythonInfo
{
    std::string libdir, instsoname, ldlibrary, home, executable, version;
    std::vector<std::string> path;
};

// one line, inside "..." for sh and cmd: no double quotes, $, ` or %
static const char *probe =
    "import sys, sysconfig; v = sysconfig.get_config_var; "
    "[print(x or '') for x in (v('LIBDIR'), v('INSTSONAME'), v('LDLIBRARY'), sys.base_prefix, "
    "sys.executable, str(sys.version_info[0]) + str(sys.version_info[1]))]; "
    "[print(p) for p in sys.path]";

static bool askPython(const std::string &exe, PythonInfo &info)
{
    FILE *f = popen(commandLine(exe, probe).c_str(), "r");
    if (f == NULL) return false;
    std::vector<std::string> lines;
    char buf[4096];
    while (fgets(buf, sizeof buf, f) != NULL)
    {
        std::string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        lines.push_back(line);
    }
    int rc = pclose(f);
    if (rc != 0 || lines.size() < 6) return false;
    info.libdir = lines[0];
    info.instsoname = lines[1];
    info.ldlibrary = lines[2];
    info.home = lines[3];
    info.executable = lines[4];
    info.version = lines[5];
    info.path.assign(lines.begin() + 6, lines.end());
    return true;
}

static bool endsWith(const std::string &s, const char *tail)
{
    size_t n = strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

#ifdef _WIN32
static std::string lastError()
{
    char buf[512];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                             GetLastError(), 0, buf, sizeof buf, NULL);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}
#endif

// load the Python's shared library, globally; the error text if impossible
static bool loadLibpython(const PythonInfo &info, std::string &error)
{
#ifdef _WIN32
    // python3X.dll (the interpreter) and python3.dll (the Stable ABI, which
    // forwards to it), both in the Python's home
    for (const std::string &name : {"python" + info.version + ".dll", std::string("python3.dll")})
    {
        std::string dll = info.home + "\\" + name;
        if (LoadLibraryExA(dll.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH) == NULL)
        {
            error = dll + ": " + lastError();
            return false;
        }
    }
    return true;
#else
    std::vector<std::string> candidates;
    if (!info.instsoname.empty()) candidates.push_back(info.libdir + "/" + info.instsoname);
    if (endsWith(info.ldlibrary, ".so") || endsWith(info.ldlibrary, ".dylib"))
    {
        candidates.push_back(info.libdir + "/" + info.ldlibrary);
    }
    for (const std::string &c : candidates)
    {
        if (dlopen(c.c_str(), RTLD_NOW | RTLD_GLOBAL) != NULL) return true;
        error = dlerror();
    }
    if (candidates.empty())
    {
        error = "this Python has no shared library (it was built statically: LDLIBRARY=" +
                info.ldlibrary + ")";
    }
    return false;
#endif
}

static bool pythonInProcess()
{
#ifdef _WIN32
    HMODULE h = GetModuleHandleA("python3.dll");
    return h != NULL && GetProcAddress(h, "Py_IsInitialized") != NULL;
#else
    return dlsym(RTLD_DEFAULT, "Py_IsInitialized") != NULL;
#endif
}

BEGIN_EXTERN_C()

LOADER_EXPORT
RexxPackageEntry *RexxEntry RexxGetPackage(void)
{
    static RexxPackageEntry *entry = NULL;      // ooRexx may ask more than once
    if (entry != NULL) return entry;

    bool pythonPresent = pythonInProcess();
    PythonInfo info;
    if (!pythonPresent)
    {
        const char *chosen = getenv("ORX_PYTHON");
        std::string exe = chosen != NULL && *chosen != '\0' ? chosen : defaultPython;
        if (!askPython(exe, info))
        {
            complain("cannot run the Python executable \"" + exe +
                     "\" (set ORX_PYTHON to the Python to use)");
            return NULL;
        }
        std::string error;
        if (!loadLibpython(info, error))
        {
            complain("cannot load the library of " + info.executable + ": " + error);
            return NULL;
        }
    }

    std::string bridgePath = ownDirectory() + "/" + bridgeFile;
#ifdef _WIN32
    for (char &c : bridgePath) if (c == '/') c = '\\';
    HMODULE bridge = LoadLibraryExA(bridgePath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (bridge == NULL)
    {
        complain("cannot load the bridge " + bridgePath + ": " + lastError());
        return NULL;
    }
    BridgePackageFn package = (BridgePackageFn)GetProcAddress(bridge, "orxpy_package");
    BridgeLaunchFn launch = (BridgeLaunchFn)GetProcAddress(bridge, "orxpy_launch");
#else
    void *bridge = dlopen(bridgePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (bridge == NULL)
    {
        complain(std::string("cannot load the bridge: ") + dlerror());
        return NULL;
    }
    BridgePackageFn package = (BridgePackageFn)dlsym(bridge, "orxpy_package");
    BridgeLaunchFn launch = (BridgeLaunchFn)dlsym(bridge, "orxpy_launch");
#endif
    if (package == NULL || launch == NULL)
    {
        complain("the bridge " + bridgePath + " is not the expected one");
        return NULL;
    }
    if (!pythonPresent)
    {
        std::vector<const char *> path;
        for (const std::string &p : info.path) path.push_back(p.c_str());
        launch(info.home.c_str(), info.executable.c_str(), path.data(), (int)path.size());
    }
    entry = package();
    return entry;
}

END_EXTERN_C()
