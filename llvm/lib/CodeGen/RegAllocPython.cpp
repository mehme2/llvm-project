#include "RegAllocGraphSolvers.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/ErrorHandling.h"

#include <iostream>

static llvm::cl::opt<std::string> PythonLibPath("python-lib", llvm::cl::init(""), llvm::cl::Hidden, llvm::cl::desc("python library path"));
static llvm::cl::opt<std::string> PythonModuleName("python-module", llvm::cl::init(""), llvm::cl::Hidden, llvm::cl::desc("python module that contains graph_color"));
static llvm::cl::opt<std::string> PythonSitePackagesPath("python-site-packages", llvm::cl::init(""), llvm::cl::Hidden, llvm::cl::desc("path to site-packages"));

struct PyObject;

typedef ssize_t Py_ssize_t;

static bool IsPythonLoaded = false;
PyObject *GraphColorFunction = NULL;

PyObject *Py_True = NULL;
PyObject *Py_False = NULL;

#define PY_FUNC_TABLE \
    PY_FUNC(void, Py_Initialize, void) \
    PY_FUNC(void, Py_FinalizeEx, void) \
    PY_FUNC(PyObject *, PyImport_ImportModule, const char *name) \
    PY_FUNC(PyObject *, PyObject_CallFunction, PyObject *callable, const char *format, ...) \
    PY_FUNC(int, PyRun_SimpleString, const char *command) \
    PY_FUNC(PyObject *, PyObject_GetAttrString, PyObject *o, const char *attr_name) \
    PY_FUNC(PyObject *, PyObject_CallFunctionObjArgs, PyObject *callable, ...) \
    PY_FUNC(PyObject *, PyObject_CallMethod, PyObject *obj, const char *name, const char *format, ...) \
    PY_FUNC(void, Py_DecRef, PyObject *o) \
    PY_FUNC(int, PyCallable_Check, PyObject *o) \
    PY_FUNC(PyObject *, PyTuple_New, Py_ssize_t len) \
    PY_FUNC(int, PyTuple_SetItem, PyObject *p, Py_ssize_t pos, PyObject *o) \
    PY_FUNC(PyObject *, PyBool_FromLong, long v) \
    PY_FUNC(PyObject *, PyFloat_FromDouble, double v) \
    PY_FUNC(Py_ssize_t, PyTuple_Size, PyObject *p) \
    PY_FUNC(int, PyLong_AsInt, PyObject *obj) \
    PY_FUNC(PyObject *, PyTuple_GetItem, PyObject *p, Py_ssize_t pos) \

#define PY_FUNC(ReturnType, Name, ...) \
    typedef ReturnType (*PyFuncPtr_##Name)(__VA_ARGS__); \
    static PyFuncPtr_##Name Name;
PY_FUNC_TABLE
#undef PY_FUNC

static void FinalizePython()
{
    Py_FinalizeEx();
}

class PythonFinalizer
{
    public:
    ~PythonFinalizer()
    {
        if(IsPythonLoaded)
        {
            FinalizePython();
        }
    }
};

static PythonFinalizer Finalizer;

static void LoadPython()
{
    llvm::sys::DynamicLibrary PythonLib;

    if(PythonLibPath.empty())
    {
        for(int VersionIndex = 9;
            !PythonLib.isValid() && (VersionIndex >= 0);
            --VersionIndex)
        {
            std::string PythonLibNameUnix = "libpython3.1";
            PythonLibNameUnix += std::to_string(VersionIndex);
            PythonLibNameUnix += ".so";
            PythonLib = PythonLib.getPermanentLibrary(PythonLibNameUnix.c_str());

            if(!PythonLib.isValid())
            {
                std::string PythonLibNameWindows = "python31";
                PythonLibNameWindows += std::to_string(VersionIndex);
                PythonLib = PythonLib.getPermanentLibrary(PythonLibNameWindows.c_str());
            }
        }
    }
    else
    {
        PythonLib = PythonLib.getPermanentLibrary(PythonLibPath.c_str());
    }

    if(!PythonLib.isValid())
    {
        llvm::reportFatalUsageError("Could not load python library");
    }

#define PY_FUNC(ReturnType, Name, ...) \
    Name = (PyFuncPtr_##Name)PythonLib.getAddressOfSymbol(#Name); \
    if(Name == NULL) \
    { \
        llvm::reportFatalUsageError("Could not load function " #Name); \
    }
PY_FUNC_TABLE
#undef PY_FUNC

    Py_Initialize();

    if(!PythonSitePackagesPath.empty())
    {
        std::string CommandToRun = "import sys\nsys.path.insert(0, '";
        CommandToRun += PythonSitePackagesPath.c_str();
        CommandToRun += "')";
        PyRun_SimpleString(CommandToRun.c_str());
    }

    Py_False = PyBool_FromLong(0);
    Py_True = PyBool_FromLong(1);

    /*
    PyObject *ImportLib = PyImport_ImportModule("importlib.util");
    PyObject *SpecFromFileLocation = PyObject_GetAttrString(ImportLib, "spec_from_file_location");
    PyObject *Spec = PyObject_CallFunction(SpecFromFileLocation, "ss", "gcmodule", PythonModulePath.c_str());
    PyObject *ModuleFromSpec = PyObject_GetAttrString(ImportLib, "module_from_spec");
    PyObject *Module = PyObject_CallFunctionObjArgs(ModuleFromSpec, Spec, NULL);
    PyObject *Loader = PyObject_GetAttrString(Spec, "loader");
    PyObject *Result = PyObject_CallMethod(Loader, "exec_module", "O", Module);
    */

    PyObject *Module = PyImport_ImportModule(PythonModuleName.c_str());

    GraphColorFunction = PyObject_GetAttrString(Module, "graph_color");

    if(GraphColorFunction == NULL)
    {
        FinalizePython();
        llvm::reportFatalUsageError("Could not load specified module");
    }

    /*
    Py_DecRef(Result);
    Py_DecRef(Loader);
    Py_DecRef(ModuleFromSpec);
    Py_DecRef(Spec);
    Py_DecRef(SpecFromFileLocation);
    Py_DecRef(ImportLib);
    */
    Py_DecRef(Module);

    if(PyCallable_Check(GraphColorFunction) == 0)
    {
        FinalizePython();
        llvm::reportFatalUsageError("Could not load graph_color");
    }

    IsPythonLoaded = true;
}

REGALLOC_GRAPH_SOLVER(RegAllocPythonSolver)
{
    if(!IsPythonLoaded)
    {
        LoadPython();
    }

    unsigned VertCount = Graph.getVertexCount();

    PyObject *WeightsTuple = PyTuple_New(VertCount);

    for(unsigned VertIndex = 0;
        VertIndex < VertCount;
        ++VertIndex)
    {
        float Weight;
        if(Graph.isPhys(VertIndex))
        {
            Weight = 10000000.0f;
        }
        else
        {
            unsigned VirtIndex = Graph.vertIndexToVirtIndex(VertIndex);
            if(Graph.isSpillable(VirtIndex))
            {
               Weight = Graph.getWeight(VirtIndex);
            }
            else Weight = 10000.0f;
        }

        PyObject *WeightObj = PyFloat_FromDouble(Weight);
        PyTuple_SetItem(WeightsTuple, VertIndex, WeightObj);
    }

    PyObject *EdgesTuple = PyTuple_New(VertCount);

    for(unsigned VertIndexA = 0;
        VertIndexA < VertCount;
        ++VertIndexA)
    {
        PyObject *RowTuple = PyTuple_New(VertCount);
        for(unsigned VertIndexB = 0;
            VertIndexB < VertCount;
            ++VertIndexB)
        {
            PyTuple_SetItem(RowTuple, VertIndexB,
                            Graph.hasEdge(VertIndexA, VertIndexB) ? Py_True : Py_False);
        }
        PyTuple_SetItem(EdgesTuple, VertIndexA, RowTuple);
    }

    PyObject *SolutionTuple = PyObject_CallFunctionObjArgs(GraphColorFunction, EdgesTuple, WeightsTuple, NULL);

    unsigned VirtCount = Graph.getVirtRegCount();
    std::vector<int> Solution(VirtCount, -1);

    if(//PyTuple_Check(SolutionTuple) &&
       (PyTuple_Size(SolutionTuple) == VertCount))
    {
        unsigned PhysCount = Graph.getPhysRegCount();
        std::vector<int> ColorToPhys(PhysCount, -1);

        for(unsigned PhysIndex = 0;
            PhysIndex < PhysCount;
            ++PhysIndex)
        {
            unsigned VertIndex = Graph.physIndexToVertIndex(PhysIndex);
            int Color = PyLong_AsInt(PyTuple_GetItem(SolutionTuple, VertIndex));
            if((Color >= 0) &&
               (Color < (int)PhysCount))
            {
                ColorToPhys[Color] = PhysIndex;
            }
        }

        for(unsigned VirtIndex = 0;
            VirtIndex < VirtCount;
            ++VirtIndex)
        {
            unsigned VertIndex = Graph.virtIndexToVertIndex(VirtIndex);
            int Color = PyLong_AsInt(PyTuple_GetItem(SolutionTuple, VertIndex));

            if((Color >= 0) &&
               (Color < (int)PhysCount))
            {
                Solution[VirtIndex] = ColorToPhys[Color];
            }
        }
    }
    else
    {
        // TODO: warning
    }

    Py_DecRef(SolutionTuple);
    Py_DecRef(WeightsTuple);
    Py_DecRef(EdgesTuple);

    return Solution;
}
