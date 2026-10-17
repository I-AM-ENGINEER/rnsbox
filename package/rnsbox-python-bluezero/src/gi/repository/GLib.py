# Minimal gi.repository.GLib shim for bluezero on systems without PyGObject.
#
# bluezero imports this module at load time and uses exactly three things:
# GLib.MainLoop (run/quit/is_running), GLib.timeout_add and
# GLib.timeout_add_seconds. The D-Bus main-loop integration itself is the
# C extension dbus-python ships (_dbus_glib_bindings, registered through
# dbus.mainloop.glib.DBusGMainLoop) against the very same libglib-2.0, so
# this shim only needs to run the GLib default main context and register
# timeouts via ctypes. That keeps the GATT-server path working on musl
# (PyGObject/gobject-introspection cannot build there) and on glibc alike,
# with no extra packages.

import ctypes

_lib = None
for _name in ("libglib-2.0.so.0", "libglib-2.0.so"):
    try:
        _lib = ctypes.CDLL(_name)
        break
    except OSError:
        continue

if _lib is not None:
    _lib.g_main_loop_new.restype = ctypes.c_void_p
    _lib.g_main_loop_new.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _lib.g_main_loop_run.restype = None
    _lib.g_main_loop_run.argtypes = [ctypes.c_void_p]
    _lib.g_main_loop_quit.restype = None
    _lib.g_main_loop_quit.argtypes = [ctypes.c_void_p]
    _lib.g_main_loop_is_running.restype = ctypes.c_int
    _lib.g_main_loop_is_running.argtypes = [ctypes.c_void_p]
    _lib.g_timeout_add.restype = ctypes.c_uint
    _lib.g_timeout_add.argtypes = [ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
    _lib.g_timeout_add_seconds.restype = ctypes.c_uint
    _lib.g_timeout_add_seconds.argtypes = [ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
    _lib.g_source_remove.restype = ctypes.c_int
    _lib.g_source_remove.argtypes = [ctypes.c_uint]

_CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
# The C side keeps only the raw function pointer, so the Python callable
# objects must stay referenced here or they would be garbage-collected
# while the timeout source is still armed.
_keep = {}


def _add(adder, interval, callback, data):
    def _trampoline(_):
        try:
            ret = callback() if data is None else callback(data)
        except Exception:
            return 0  # drop the source, like an exception in GLib would
        if ret is None:
            return 1  # periodic timer, keep firing
        return 1 if ret else 0

    cb = _CB(_trampoline)
    _keep[cb] = cb
    return int(adder(int(interval), cb, None))


def timeout_add(interval, callback, *args):
    """Call callback every interval milliseconds; returns the source id."""
    return _add(_lib.g_timeout_add, interval, callback,
                args[0] if args else None)


def timeout_add_seconds(interval, callback, *args):
    """Call callback every interval seconds; returns the source id."""
    return _add(_lib.g_timeout_add_seconds, interval, callback,
                args[0] if args else None)


def source_remove(source_id):
    return bool(_lib.g_source_remove(int(source_id)))


class MainLoop:
    """GLib.MainLoop over the default main context."""

    def __init__(self, context=None, is_running=False):
        self._loop = _lib.g_main_loop_new(None, 1 if is_running else 0)

    def run(self):
        _lib.g_main_loop_run(self._loop)

    def quit(self):
        _lib.g_main_loop_quit(self._loop)

    def is_running(self):
        return bool(_lib.g_main_loop_is_running(self._loop))

    def get_context(self):
        return None
