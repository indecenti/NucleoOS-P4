#!/usr/bin/env python3
"""Embed a built WASM app as a C seed header so the OS can self-install it at boot.

Some first-party WASM apps ship *inside* the firmware and write themselves to /sdcard/apps/<id>/ on
boot (the sandboxed web-FS can't reach that dir). This turns apps/<id>/{manifest.json,app.wasm} into
components/nv_wasm/<id>_seed.inc with the symbols nv_wasm_seed_<id>() expects.

    python tools/gen_wasm_seed.py tanks

Emits kTanksVersion / kTanksManifest / kTanksWasm[] / kTanksWasmLen (identifier = Titlecased id).
Bump the app's manifest "version" so the on-device version check reseeds it.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def c_string(text):
    """Render `text` as a sequence of C string literals, one per source line (\\n kept)."""
    out = []
    for line in text.splitlines(keepends=True):
        esc = line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
        out.append(f'    "{esc}"')
    return "\n".join(out) + "\n    ;"


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: gen_wasm_seed.py <app-id>")
    app_id = sys.argv[1]
    sym = app_id.capitalize()                     # tanks -> Tanks
    app_dir = os.path.join(ROOT, "apps", app_id)
    man_path = os.path.join(app_dir, "manifest.json")
    wasm_path = os.path.join(app_dir, "app.wasm")
    out_path = os.path.join(ROOT, "components", "nv_wasm", f"{app_id}_seed.inc")

    manifest_raw = open(man_path, "r", encoding="utf-8").read()
    version = str(json.loads(manifest_raw).get("version", "1.0"))
    wasm = open(wasm_path, "rb").read()

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"// AUTO-GENERATED from apps/{app_id}/ by tools/gen_wasm_seed.py. Embedded so the OS\n")
        f.write(f"// seeds it to /sdcard/apps/{app_id}. Do not edit; re-run the generator instead.\n")
        f.write(f'static const char k{sym}Version[] = "{version}";\n')
        f.write(f"static const char k{sym}Manifest[] =\n{c_string(manifest_raw)}\n")
        f.write(f"static const unsigned char k{sym}Wasm[] = {{\n")
        for i in range(0, len(wasm), 16):
            f.write("    " + ",".join(str(b) for b in wasm[i:i + 16]) + ",\n")
        f.write("};\n")
        f.write(f"static const unsigned k{sym}WasmLen = {len(wasm)};\n")

    print(f"wrote {out_path}  (v{version}, {len(wasm)} bytes)")


if __name__ == "__main__":
    main()
