/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * loader.cpp: librexxpython.so, the ooRexx native package of the ooRexx/Python
 * bridge (docs/design.md, *Two libraries*).  It holds no bridge code: it makes sure a Python is in
 * the process and then hands ooRexx the package of the real bridge,
 * oorexx.abi3.so (the same file Python imports as the module "oorexx").
 *
 *  - If Python is already in the process (a Python program running Rexx code
 *    that requires python.cls), it is used: nothing else is loaded.
 *  - Otherwise the Python to use is found (ORX_PYTHON, else "python3" on the
 *    PATH), asked for its library, home and sys.path, and its libpython is
 *    loaded with RTLD_GLOBAL, so that C extension modules (numpy...) find the
 *    Py* symbols.  Only then is the bridge loaded: its Py* references can be
 *    bound.  The bridge initializes Python when ooRexx loads the package.
 *
 * Platform-dependent by nature (docs/design.md, *Platforms*): Linux now; macOS and Windows later.
 */

#include "oorexxapi.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

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
    Dl_info info;
    if (dladdr((void *)&ownDirectory, &info) == 0 || info.dli_fname == NULL)
    {
        return ".";
    }
    std::string path = info.dli_fname;
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// a string as a single-quoted shell word
static std::string shellQuote(const std::string &s)
{
    std::string q = "'";
    for (char c : s)
    {
        if (c == '\'') q += "'\\''"; else q += c;
    }
    return q + "'";
}

// what the chosen Python executable says about itself
struct PythonInfo
{
    std::string libdir, instsoname, ldlibrary, home, executable;
    std::vector<std::string> path;
};

static const char *probe =
    "import sys, sysconfig\n"
    "v = sysconfig.get_config_var\n"
    "for x in (v('LIBDIR'), v('INSTSONAME'), v('LDLIBRARY'), sys.base_prefix, sys.executable):\n"
    "    print(x or '')\n"
    "for p in sys.path:\n"
    "    print(p)\n";

static bool askPython(const std::string &exe, PythonInfo &info)
{
    std::string cmd = shellQuote(exe) + " -c " + shellQuote(probe) + " 2>/dev/null";
    FILE *f = popen(cmd.c_str(), "r");
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
    if (rc != 0 || lines.size() < 5) return false;
    info.libdir = lines[0];
    info.instsoname = lines[1];
    info.ldlibrary = lines[2];
    info.home = lines[3];
    info.executable = lines[4];
    info.path.assign(lines.begin() + 5, lines.end());
    return true;
}

static bool endsWith(const std::string &s, const char *tail)
{
    size_t n = strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// load the Python's shared library, globally; the error text if impossible
static bool loadLibpython(const PythonInfo &info, std::string &error)
{
    std::vector<std::string> candidates;
    if (!info.instsoname.empty()) candidates.push_back(info.libdir + "/" + info.instsoname);
    if (endsWith(info.ldlibrary, ".so")) candidates.push_back(info.libdir + "/" + info.ldlibrary);
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
}

BEGIN_EXTERN_C()

__attribute__((visibility("default")))
RexxPackageEntry *RexxGetPackage(void)
{
    static RexxPackageEntry *entry = NULL;      // ooRexx may ask more than once
    if (entry != NULL) return entry;

    bool pythonPresent = dlsym(RTLD_DEFAULT, "Py_IsInitialized") != NULL;
    PythonInfo info;
    if (!pythonPresent)
    {
        const char *chosen = getenv("ORX_PYTHON");
        std::string exe = chosen != NULL && *chosen != '\0' ? chosen : "python3";
        if (!askPython(exe, info))
        {
            complain("cannot run the Python executable \"" + exe +
                     "\" (set ORX_PYTHON to the python3 to use)");
            return NULL;
        }
        std::string error;
        if (!loadLibpython(info, error))
        {
            complain("cannot load the library of " + info.executable + ": " + error);
            return NULL;
        }
    }

    std::string bridgePath = ownDirectory() + "/oorexx.abi3.so";
    void *bridge = dlopen(bridgePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (bridge == NULL)
    {
        complain(std::string("cannot load the bridge: ") + dlerror());
        return NULL;
    }
    BridgePackageFn package = (BridgePackageFn)dlsym(bridge, "orxpy_package");
    BridgeLaunchFn launch = (BridgeLaunchFn)dlsym(bridge, "orxpy_launch");
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
