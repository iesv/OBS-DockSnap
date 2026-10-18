"""Load the packaged module using only the official OBS runtime dependencies."""
import ctypes
import os
import sys
from pathlib import Path

binary, runtime = (Path(argument).resolve() for argument in sys.argv[1:])
if sys.platform == "win32":
    ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    directory = os.add_dll_directory(str(runtime / "bin/64bit"))
    module = ctypes.CDLL(str(binary))
else:
    module = ctypes.CDLL(str(binary))
module.obs_module_ver.restype = ctypes.c_uint32
module.obs_module_name.restype = ctypes.c_char_p
if module.obs_module_ver() >> 24 != 32 or module.obs_module_name() != b"OBS-DockSnap":
    raise RuntimeError("Unexpected module ABI or name")
for symbol in ("obs_module_load", "obs_module_unload", "obs_module_set_pointer",
               "obs_module_set_locale", "obs_module_free_locale", "obs_module_get_string"):
    getattr(module, symbol)
module.obs_module_load.restype = ctypes.c_bool
if module.obs_module_load():
    raise RuntimeError("Module must reject a missing frontend window")
module.obs_module_unload()
module.obs_module_unload()
print("PASS: packaged module loads with OBS 32.0.4 runtime dependencies; public module exports present")
