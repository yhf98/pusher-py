#include <Python.h>

#include "pusher/pusher.hpp"
#include "pusher/url_utils.h"

#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char *kVersion = "0.1.9";

#define PUSHER_METHOD_CAST(func) \
    reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(func))

typedef struct {
    PyObject_HEAD
    pusher::NativePusher *impl;
} PyNativePusher;

static PyObject *set_python_error(const std::exception &error) {
    PyErr_SetString(PyExc_RuntimeError, error.what());
    return nullptr;
}

static PyObject *py_version(PyObject *, PyObject *) {
    return PyUnicode_FromString(kVersion);
}

static PyObject *py_detect_protocol(PyObject *, PyObject *args) {
    const char *url = nullptr;
    if (!PyArg_ParseTuple(args, "s", &url)) {
        return nullptr;
    }
    return PyUnicode_FromString(pusher_detect_protocol(url));
}

static PyObject *py_build_output_url(PyObject *, PyObject *args, PyObject *kwargs) {
    const char *protocol = nullptr;
    const char *host = nullptr;
    const char *app = "live";
    const char *stream = "test";
    const char *secret = nullptr;
    int port = 0;
    int use_tls = 0;

    static char *kwlist[] = {
        const_cast<char *>("protocol"),
        const_cast<char *>("host"),
        const_cast<char *>("app"),
        const_cast<char *>("stream"),
        const_cast<char *>("secret"),
        const_cast<char *>("port"),
        const_cast<char *>("use_tls"),
        nullptr,
    };

    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "ss|sszip", kwlist,
            &protocol, &host, &app, &stream, &secret, &port, &use_tls)) {
        return nullptr;
    }

    try {
        std::string url = pusher::build_output_url(
            protocol,
            host,
            app,
            stream,
            secret == nullptr ? "" : secret,
            port,
            use_tls != 0);
        return PyUnicode_FromString(url.c_str());
    } catch (const std::invalid_argument &error) {
        PyErr_SetString(PyExc_ValueError, error.what());
        return nullptr;
    } catch (const std::exception &error) {
        return set_python_error(error);
    }
}

static PyObject *vector_to_pylist(const std::vector<std::string> &items) {
    PyObject *list = PyList_New(static_cast<Py_ssize_t>(items.size()));
    if (list == nullptr) {
        return nullptr;
    }

    for (size_t i = 0; i < items.size(); ++i) {
        PyObject *value = PyUnicode_FromString(items[i].c_str());
        if (value == nullptr) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), value);
    }

    return list;
}

static PyObject *PyNativePusher_new(PyTypeObject *type, PyObject *, PyObject *) {
    PyNativePusher *self = reinterpret_cast<PyNativePusher *>(type->tp_alloc(type, 0));
    if (self != nullptr) {
        self->impl = nullptr;
    }
    return reinterpret_cast<PyObject *>(self);
}

static int PyNativePusher_init(PyNativePusher *self, PyObject *args, PyObject *kwargs) {
    const char *name = "default";
    int timeout_ms = 5000;
    int auto_reconnect = 1;
    const char *engine = "auto";
    const char *log_path = "";
    int loop = 1;
    int realtime = 1;
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 2000000;
    long long analyzeduration_us = 10000000;
    long long probesize = 50000000;

    static char *kwlist[] = {
        const_cast<char *>("name"),
        const_cast<char *>("timeout_ms"),
        const_cast<char *>("auto_reconnect"),
        const_cast<char *>("engine"),
        const_cast<char *>("log_path"),
        const_cast<char *>("loop"),
        const_cast<char *>("realtime"),
        const_cast<char *>("width"),
        const_cast<char *>("height"),
        const_cast<char *>("fps"),
        const_cast<char *>("bitrate"),
        const_cast<char *>("analyzeduration_us"),
        const_cast<char *>("probesize"),
        nullptr,
    };

    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "|sipszppiiiiLL", kwlist,
            &name, &timeout_ms, &auto_reconnect,
            &engine, &log_path, &loop, &realtime,
            &width, &height, &fps, &bitrate, &analyzeduration_us, &probesize)) {
        return -1;
    }

    delete self->impl;
    self->impl = nullptr;

    try {
        pusher::PusherConfig config;
        config.name = name;
        config.timeout_ms = timeout_ms;
        config.auto_reconnect = auto_reconnect != 0;
        config.engine = engine == nullptr ? "auto" : engine;
        config.log_path = log_path == nullptr ? "" : log_path;
        config.loop = loop != 0;
        config.realtime = realtime != 0;
        config.width = width;
        config.height = height;
        config.fps = fps;
        config.bitrate = bitrate;
        config.analyzeduration_us = analyzeduration_us;
        config.probesize = probesize;
        self->impl = new pusher::NativePusher(config);
    } catch (const std::invalid_argument &error) {
        PyErr_SetString(PyExc_ValueError, error.what());
        return -1;
    } catch (const std::bad_alloc &) {
        PyErr_NoMemory();
        return -1;
    } catch (const std::exception &error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
        return -1;
    }

    return 0;
}

static void PyNativePusher_dealloc(PyNativePusher *self) {
    delete self->impl;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

static PyObject *PyNativePusher_start(PyNativePusher *self, PyObject *args, PyObject *kwargs) {
    const char *input = nullptr;
    const char *output_url = nullptr;

    static char *kwlist[] = {
        const_cast<char *>("input"),
        const_cast<char *>("output_url"),
        nullptr,
    };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss", kwlist, &input, &output_url)) {
        return nullptr;
    }

    try {
        self->impl->start(input, output_url);
        Py_RETURN_NONE;
    } catch (const std::invalid_argument &error) {
        PyErr_SetString(PyExc_ValueError, error.what());
        return nullptr;
    } catch (const std::exception &error) {
        return set_python_error(error);
    }
}

static PyObject *PyNativePusher_stop(PyNativePusher *self, PyObject *args, PyObject *kwargs) {
    int timeout_ms = -1;
    static char *kwlist[] = {
        const_cast<char *>("timeout_ms"),
        nullptr,
    };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i", kwlist, &timeout_ms)) {
        return nullptr;
    }

    try {
        self->impl->stop(timeout_ms);
        Py_RETURN_NONE;
    } catch (const std::exception &error) {
        return set_python_error(error);
    }
}

static PyObject *PyNativePusher_wait(PyNativePusher *self, PyObject *args, PyObject *kwargs) {
    int timeout_ms = -1;
    static char *kwlist[] = {
        const_cast<char *>("timeout_ms"),
        nullptr,
    };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i", kwlist, &timeout_ms)) {
        return nullptr;
    }

    try {
        int exit_code = -1;
        bool completed = self->impl->wait(timeout_ms, exit_code);
        if (!completed) {
            Py_RETURN_NONE;
        }
        return PyLong_FromLong(exit_code);
    } catch (const std::exception &error) {
        return set_python_error(error);
    }
}

static PyObject *PyNativePusher_status(PyNativePusher *self, PyObject *) {
    try {
        const std::string value = self->impl->status();
        return PyUnicode_FromString(value.c_str());
    } catch (const std::exception &error) {
        return set_python_error(error);
    }
}

static PyObject *PyNativePusher_preview_command(PyNativePusher *self, PyObject *args, PyObject *kwargs) {
    const char *input = nullptr;
    const char *output_url = nullptr;

    static char *kwlist[] = {
        const_cast<char *>("input"),
        const_cast<char *>("output_url"),
        nullptr,
    };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss", kwlist, &input, &output_url)) {
        return nullptr;
    }

    try {
        return vector_to_pylist(self->impl->preview_command(input, output_url));
    } catch (const std::invalid_argument &error) {
        PyErr_SetString(PyExc_ValueError, error.what());
        return nullptr;
    } catch (const std::exception &error) {
        return set_python_error(error);
    }
}

static PyObject *PyNativePusher_command(PyNativePusher *self, PyObject *) {
    try {
        return vector_to_pylist(self->impl->command());
    } catch (const std::exception &error) {
        return set_python_error(error);
    }
}

static PyObject *PyNativePusher_enter(PyNativePusher *self, PyObject *) {
    Py_INCREF(self);
    return reinterpret_cast<PyObject *>(self);
}

static PyObject *PyNativePusher_exit(PyNativePusher *self, PyObject *) {
    self->impl->stop();
    Py_RETURN_FALSE;
}

static PyObject *PyNativePusher_get_is_running(PyNativePusher *self, void *) {
    if (self->impl->is_running()) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *PyNativePusher_get_protocol(PyNativePusher *self, void *) {
    const std::string value = self->impl->protocol();
    return PyUnicode_FromString(value.c_str());
}

static PyObject *PyNativePusher_get_engine(PyNativePusher *self, void *) {
    const std::string value = self->impl->engine();
    return PyUnicode_FromString(value.c_str());
}

static PyObject *PyNativePusher_get_name(PyNativePusher *self, void *) {
    const std::string value = self->impl->name();
    return PyUnicode_FromString(value.c_str());
}

static PyObject *PyNativePusher_get_pid(PyNativePusher *self, void *) {
    return PyLong_FromLong(self->impl->pid());
}

static PyObject *PyNativePusher_get_exit_code(PyNativePusher *self, void *) {
    int exit_code = self->impl->exit_code();
    if (exit_code < 0) {
        Py_RETURN_NONE;
    }
    return PyLong_FromLong(exit_code);
}

static PyObject *PyNativePusher_repr(PyNativePusher *self) {
    const std::string value = "<pusher.Pusher " + self->impl->status() + ">";
    return PyUnicode_FromString(value.c_str());
}

static PyMethodDef PyNativePusher_methods[] = {
    {"start", PUSHER_METHOD_CAST(PyNativePusher_start), METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("Start a native pusher session.")},
    {"stop", PUSHER_METHOD_CAST(PyNativePusher_stop), METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("Stop the native pusher session.")},
    {"wait", PUSHER_METHOD_CAST(PyNativePusher_wait), METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("Wait for the native pusher worker. Return exit code, or None on timeout.")},
    {"status", PUSHER_METHOD_CAST(PyNativePusher_status), METH_NOARGS,
     PyDoc_STR("Return current pusher status.")},
    {"preview_command", PUSHER_METHOD_CAST(PyNativePusher_preview_command), METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("Return the command that would be executed for a push session.")},
    {"command", PUSHER_METHOD_CAST(PyNativePusher_command), METH_NOARGS,
     PyDoc_STR("Return the active command as a list of arguments.")},
    {"__enter__", PUSHER_METHOD_CAST(PyNativePusher_enter), METH_NOARGS,
     PyDoc_STR("Enter a context manager.")},
    {"__exit__", PUSHER_METHOD_CAST(PyNativePusher_exit), METH_VARARGS,
     PyDoc_STR("Leave a context manager and stop the pusher.")},
    {nullptr, nullptr, 0, nullptr},
};

static PyGetSetDef PyNativePusher_getset[] = {
    {const_cast<char *>("is_running"), reinterpret_cast<getter>(PyNativePusher_get_is_running), nullptr,
     const_cast<char *>("Whether the pusher is running."), nullptr},
    {const_cast<char *>("protocol"), reinterpret_cast<getter>(PyNativePusher_get_protocol), nullptr,
     const_cast<char *>("Detected output protocol."), nullptr},
    {const_cast<char *>("engine"), reinterpret_cast<getter>(PyNativePusher_get_engine), nullptr,
     const_cast<char *>("Selected native push engine."), nullptr},
    {const_cast<char *>("name"), reinterpret_cast<getter>(PyNativePusher_get_name), nullptr,
     const_cast<char *>("Pusher instance name."), nullptr},
    {const_cast<char *>("pid"), reinterpret_cast<getter>(PyNativePusher_get_pid), nullptr,
     const_cast<char *>("Always -1; native push runs in worker threads."), nullptr},
    {const_cast<char *>("exit_code"), reinterpret_cast<getter>(PyNativePusher_get_exit_code), nullptr,
     const_cast<char *>("Worker exit code after completion, otherwise None."), nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
static PyTypeObject PyNativePusherType = {PyVarObject_HEAD_INIT(nullptr, 0)};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static PyMethodDef module_methods[] = {
    {"version", py_version, METH_NOARGS, PyDoc_STR("Return native extension version.")},
    {"detect_protocol", py_detect_protocol, METH_VARARGS, PyDoc_STR("Detect protocol from output URL.")},
    {"build_output_url", PUSHER_METHOD_CAST(py_build_output_url), METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("Build a common streaming output URL.")},
    {nullptr, nullptr, 0, nullptr},
};

static PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "pusher._native",
    "Native C/C++ extension for pusher.",
    -1,
    module_methods,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

}  // namespace

PyMODINIT_FUNC PyInit__native(void) {
    PyNativePusherType.tp_name = "pusher._native.Pusher";
    PyNativePusherType.tp_basicsize = sizeof(PyNativePusher);
    PyNativePusherType.tp_itemsize = 0;
    PyNativePusherType.tp_dealloc = reinterpret_cast<destructor>(PyNativePusher_dealloc);
    PyNativePusherType.tp_flags = Py_TPFLAGS_DEFAULT;
    PyNativePusherType.tp_doc = PyDoc_STR("Native pusher handle implemented in C++.");
    PyNativePusherType.tp_methods = PyNativePusher_methods;
    PyNativePusherType.tp_getset = PyNativePusher_getset;
    PyNativePusherType.tp_init = reinterpret_cast<initproc>(PyNativePusher_init);
    PyNativePusherType.tp_new = PyNativePusher_new;
    PyNativePusherType.tp_repr = reinterpret_cast<reprfunc>(PyNativePusher_repr);

    if (PyType_Ready(&PyNativePusherType) < 0) {
        return nullptr;
    }

    PyObject *module = PyModule_Create(&module_def);
    if (module == nullptr) {
        return nullptr;
    }

    Py_INCREF(&PyNativePusherType);
    if (PyModule_AddObject(module, "Pusher", reinterpret_cast<PyObject *>(&PyNativePusherType)) < 0) {
        Py_DECREF(&PyNativePusherType);
        Py_DECREF(module);
        return nullptr;
    }

    PyModule_AddStringConstant(module, "__version__", kVersion);
    return module;
}
