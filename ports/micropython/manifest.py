# Frozen into app.wasm (bytecode in the image, imported like built-ins, nothing on the SD):
# - pure-Python modules from micropython-lib (MIT), shipped in the MicroPython release tarball;
# - lib/: os (+ os.walk, makedirs), os.path (posixpath API), pathlib (iterdir, fnmatch globs),
#   textwrap, written or extended for NucleoOS where micropython-lib's versions fall short.
# Keep to what works without sockets, threads or ffi.
for m in (
    "abc", "argparse", "bisect", "collections-defaultdict", "contextlib", "copy",
    "datetime", "fnmatch", "functools", "html", "hmac", "inspect", "itertools", "keyword",
    "locale", "logging", "operator", "pprint", "shutil", "stat", "string", "tarfile",
    "tempfile", "traceback", "types", "unittest", "warnings", "gzip",
):
    require(m)
# base64 without its require("binascii"): that pure-Python binascii would replace the C one.
module("base64.py", base_path="$(MPY_LIB_DIR)/python-stdlib/base64")
package("os", base_path="$(PORT_DIR)/lib")
module("pathlib.py", base_path="$(PORT_DIR)/lib")
module("textwrap.py", base_path="$(PORT_DIR)/lib")
