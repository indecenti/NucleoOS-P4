"""abtool.py — build Arduboy sketches as NucleoOS apps (runs inside WSL; ports/arduboy/build.sh
calls it). Part of ports/arduboy.

    python3 abtool.py libs                 patch the pinned Arduboy libraries, build the shim (wasm + PC)
    python3 abtool.py build [slug ...]     every game in games.py (or the given ones): wasm + AOT,
                                           PC harness run, icon, shots, manifest, guides
    python3 abtool.py test [slug ...]      PC harness only (no app files written)
    python3 abtool.py wamr [slug ...]      run the built app.wasm (and an x86-64 AOT) under WAMR
    python3 abtool.py pin slug [ref]       pin a game's repository (commit + sha256) into fetch.sh
    python3 abtool.py docs                 GAMES.md + catalog_entries.json from games.py

Layout: sources in ports/_src/arduboy (fetch.sh), generated files and test output in
ports/_src/arduboy/gen, apps in apps/ab-<slug>.
"""
import concurrent.futures as cf
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "ports", "_src", "arduboy")
GEN = os.path.join(SRC, "gen")
SHIM = os.path.join(HERE, "shim")
SDK_INC = os.path.join(ROOT, "sdk", "include")
WASI = os.environ.get("WASI", "/opt/wasi-sdk-34.0-x86_64-linux")
WAMRC = os.environ.get("WAMRC", "/root/wamrc-build/wamrc")
WAMR_LIB = "/root/nvhost-lib2/libiwasm.a"
WAMR_INC = os.path.join(ROOT, "reference", "wasm-micro-runtime", "core", "iwasm", "include")
CLANGXX = [os.path.join(WASI, "bin", "clang++"), "--target=wasm32-wasip1",
           "--sysroot=" + os.path.join(WASI, "share", "wasi-sysroot")]
CLANG = [os.path.join(WASI, "bin", "clang"), "--target=wasm32-wasip1",
         "--sysroot=" + os.path.join(WASI, "share", "wasi-sysroot")]
BUILTINS = glob.glob(os.path.join(WASI, "lib", "clang", "*", "lib", "wasm32-unknown-wasip1",
                                  "libclang_rt.builtins.a"))
FRAMES = 1200
WASM_CAP, AOT_CAP = 2 * 1024 * 1024, 4 * 1024 * 1024

sys.path.insert(0, HERE)
import games as G  # noqa: E402

INCLUDES = ["-I" + SHIM, "-I" + os.path.join(GEN, "ab2"), "-I" + os.path.join(GEN, "ab1"),
            "-I" + os.path.join(GEN, "FixedPoints"), "-I" + os.path.join(SRC, "Tinyfont", "src"), "-I" + SDK_INC]
CXX_COMMON = ["-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-std=gnu++14", "-w",
              "-ffunction-sections", "-fdata-sections"]
# -fno-strict-return: falling off the end of a non-void function returns garbage (as with avr-gcc)
# instead of trapping; -Wno-invalid-constexpr: gcc-only constexpr leniency.
WASM_CXX = CLANGXX + ["-O2", "-Wno-c++11-narrowing", "-Wno-register", "-fno-strict-return",
                      "-Wno-invalid-constexpr", "-fwrapv", "-fno-strict-aliasing"] + CXX_COMMON
WASM_CC = CLANG + ["-O2", "-w", "-std=gnu11", "-ffunction-sections", "-fdata-sections"]
NAT_CXX = ["g++", "-O1", "-g", "-DNV_SIM", "-Wno-narrowing", "-fpermissive"] + CXX_COMMON
NAT_CC = ["gcc", "-O1", "-g", "-DNV_SIM", "-w", "-std=gnu11"]
LIB_SOURCES = [os.path.join(SHIM, f) for f in ("nvab_score.cpp", "ArduboyTones.cpp")]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)


def log(*a):
    print(*a, flush=True)


# ---- libraries ------------------------------------------------------------------------------------
def lib_sources():
    g = [os.path.join(GEN, "ab2", f) for f in sorted(os.listdir(os.path.join(GEN, "ab2"))) if f.endswith(".cpp")]
    g += [os.path.join(GEN, "ab1", "Arduboy.cpp"), os.path.join(GEN, "ab1", "core", "core.cpp"),
          os.path.join(GEN, "ab1", "audio", "audio.cpp")]
    g += [os.path.join(SRC, "Tinyfont", "src", "Tinyfont.cpp")]
    return LIB_SOURCES + g


def cmd_libs():
    r = run(["bash", os.path.join(HERE, "fetch.sh"), "--libs-only-marker--"])
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    r = run([sys.executable, os.path.join(HERE, "patch_libs.py"), SRC, GEN])
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    for kind, cxx in (("wasm", WASM_CXX), ("native", NAT_CXX)):
        d = os.path.join(GEN, "lib-" + kind)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        objs = []
        for s in lib_sources():
            o = os.path.join(d, os.path.basename(s) + ".o")
            r = run(cxx + INCLUDES + ["-c", s, "-o", o])
            if r.returncode:
                sys.exit(f"{s}:\n{r.stderr}")
            objs.append(o)
        ar = os.path.join(WASI, "bin", "llvm-ar") if kind == "wasm" else "ar"
        r = run([ar, "rcs", os.path.join(d, "libab.a")] + objs)
        if r.returncode:
            sys.exit(r.stderr)
        r = run(cxx + INCLUDES + ["-c", os.path.join(SHIM, "nvab.cpp"), "-o", os.path.join(d, "nvab.o")])
        if r.returncode:
            sys.exit(r.stderr)
    log("libs ready:", GEN)


# ---- sketch preparation -------------------------------------------------------------------------------
KEYWORDS = {"if", "while", "for", "switch", "return", "else", "do", "sizeof", "catch", "defined"}


def strip_code(s):
    """Blank out comments, strings and char literals (same length) so brace scanning is safe."""
    out = list(s)
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if s.startswith("//", i):
            j = s.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and s[j] != c:
                j += 2 if s[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def find_prototypes(code):
    """Top-level function definitions of a concatenated .ino: (insert_pos, [prototypes])."""
    s = strip_code(code)
    # blank preprocessor lines
    s = re.sub(r"^[ \t]*#.*?(?<!\\)$", lambda m: " " * len(m.group(0)), s, flags=re.M | re.S)
    raw = strip_code(code)
    # conditional-compilation depth at each line start; only the first branch of #if/#else is
    # scanned (both usually open the same braces) and #if 0 blocks are skipped
    cond = [0] * (len(code) + 1)
    stack = []            # [is_zero, in_else]
    pos = 0
    sl = list(s)
    for line in raw.split("\n"):
        t = line.strip()
        if re.match(r"#\s*(if|ifdef|ifndef)\b", t):
            stack.append([bool(re.match(r"#\s*if\s+0\b", t)), False])
        elif re.match(r"#\s*(else|elif)\b", t) and stack:
            stack[-1][1] = True
        elif re.match(r"#\s*endif\b", t) and stack:
            stack.pop()
        skip = any(z != e for z, e in stack)
        for k in range(pos, pos + len(line) + 1):
            cond[k] = len(stack)
            if skip and k < len(sl) and sl[k] != "\n":
                sl[k] = " "
        pos += len(line) + 1
    s = "".join(sl)
    protos, first, safe = [], None, 0
    depth, stmt_start, i, n = 0, 0, 0, len(s)
    paren = 0
    while i < n:
        c = s[i]
        if c == "\n" and depth == 0 and paren == 0 and cond[i] == 0 and first is None and not s[stmt_start:i].strip():
            safe = i + 1   # a line boundary at top level outside any #if: prototypes may go here
        if c == "(":
            paren += 1
        elif c == ")":
            paren -= 1
        elif c == "{":
            if depth == 0 and paren == 0:
                head = s[stmt_start:i].strip()
                tm = re.match(r"^template\s*<[^{};]*?>\s*(.*)$", head, re.S)   # function templates too
                core = tm.group(1) if tm else head
                m = re.match(r"^((?:[\w:<>,\*&\s]|\[\[.*?\]\])+?)\b([A-Za-z_]\w*)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*"
                             r"(?:const\s*)?$", core, re.S)
                if m and m.group(2) not in KEYWORDS and "::" not in m.group(1) + m.group(2) \
                        and not re.search(r"\b(class|struct|union|enum|namespace|template|typedef|operator)\b", core) \
                        and "=" not in m.group(3) and m.group(1).strip() and \
                        not re.search(r"\b(if|while|for|switch|return|else)\b", m.group(1)):
                    if cond[i] == 0:
                        real = re.sub(r"\s+", " ", s[stmt_start:i]).strip()
                        if first is None:
                            first = safe
                        protos.append(real + ";")
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                stmt_start = i + 1
        elif c == ";" and depth == 0 and paren == 0:
            stmt_start = i + 1
        i += 1
    return first, protos


def ino_only_preprocessed(code, g, src):
    """The merged .ino run through the real preprocessor (so #if/#ifdef are resolved like in the
    Arduino builder), keeping only the lines that came from the .ino files. None if it fails."""
    tmp = os.path.join(src, "sketch_pp_in.cpp")
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write("#include <Arduino.h>\n" + code)
    defs = ["-D" + d for d in g.get("defines", [])]
    r = run(CLANGXX + ["-E", "-w", "-std=gnu++14"] + INCLUDES + ["-I" + src] + defs + [tmp])
    os.remove(tmp)
    if r.returncode:
        return None
    out, keep = [], False
    for line in r.stdout.split("\n"):
        m = re.match(r'^#\s*(?:line\s+)?\d+\s+"([^"]*)"', line)
        if m:
            keep = m.group(1).endswith(".ino")
            out.append("")
            continue
        out.append(line if keep else "")
    return "\n".join(out)


LIB_FILES = {"Arduboy2.h", "Arduboy2.cpp", "Arduboy2Core.h", "Arduboy2Core.cpp", "Arduboy2Audio.h", "Arduboy2Audio.cpp",
             "Arduboy2Beep.h", "Arduboy2Beep.cpp", "Arduboy2Data.cpp", "Sprites.h", "Sprites.cpp", "SpritesB.h",
             "SpritesB.cpp", "SpritesCommon.h", "ArduboyTones.h", "ArduboyTones.cpp", "ArduboyTonesPitches.h",
             "ArduboyPlaytune.h", "ArduboyPlaytune.cpp", "Arduboy.h", "Arduboy.cpp", "glcdfont.c", "ab_logo.c",
             "core.h", "core.cpp", "audio.h", "audio.cpp"}
LIB_MARKERS = re.compile(r"Scott Allen|Arduboy LLC|Kevin \"Arduboy\" Bates|class Arduboy2Base|class ArduboyCore|"
                         r"class ArduboyTones|class ArduboyPlaytune|class Arduboy\b|class Sprites\b|class SpritesB\b|"
                         r"glcdfont|arduboy_logo|Arduboy2Audio::|Arduboy2Base::|Arduboy2Core::|ArduboyCore::|"
                         r"Sprites::|ArduboyTones::|ArduboyAudio::|ArduboyTunes::")


def drop_bundled_libs(src):
    """Some repositories carry their own copy of the Arduboy libraries (with AVR assembly) next to the
    sketch: remove those copies so the NucleoOS ones are used. Game files that merely share a name
    (a game's own Sprites.cpp) are kept: only files that look like the library go."""
    fwd = {"core.h": "core/core.h", "audio.h": "audio/audio.h"}
    for dp, _, fn in os.walk(src):
        for f in fn:
            # sketch-local copies of ArduboyTones (timer interrupts): use the NucleoOS one
            if f.endswith(".h") and f not in LIB_FILES:
                p = os.path.join(dp, f)
                t = open(p, encoding="utf-8", errors="replace").read()
                m = re.search(r"^class\s+(ArduboyTones\w*)\b[^;\n]*$", t, re.M)
                if m and re.search(r"static\s+void\s+tones\s*\(", t):
                    cls = m.group(1)
                    body = "#pragma once\n"
                    for pf in sorted(os.listdir(dp)):   # the sketch's own note table (extra names) first
                        if pf.endswith("Pitches.h"):
                            body += '#include "%s"\n' % pf
                    body += "#include <ArduboyTones.h>\n"
                    if cls != "ArduboyTones":
                        body += ("class %s : public ArduboyTones {\npublic:\n    %s(bool (*outEn)()) : ArduboyTones(outEn) {}\n"
                                 "    %s() : ArduboyTones(nullptr) {}\n"
                                 "    static void setOutputEnabled(bool (*outEn)()) { ArduboyTones::setOutputEnabled(outEn); }\n"
                                 "};\n" % (cls, cls, cls))
                    with open(p, "w", encoding="utf-8", newline="\n") as fh:
                        fh.write("// NucleoOS: this sketch's ArduboyTones copy is replaced by ports/arduboy/shim\n" + body)
                    stem = os.path.join(dp, f[:-2] + ".cpp")
                    if os.path.exists(stem):
                        os.remove(stem)
                    continue
            if f in LIB_FILES:
                p = os.path.join(dp, f)
                t = open(p, encoding="utf-8", errors="replace").read(20000)
                if not LIB_MARKERS.search(t):
                    continue
                if f.endswith(".h"):   # keep relative includes working: forward to the NucleoOS copy
                    with open(p, "w", encoding="utf-8", newline="\n") as fh:
                        fh.write("#pragma once\n#include <%s>\n" % fwd.get(f, f))
                else:
                    os.remove(p)


def fix_static_conflicts(src):
    """avr-gcc (-fpermissive) accepts a function declared in a header and then `static` in a .cpp;
    clang does not. Drop that `static` in the .cpp files."""
    names = set()
    for dp, _, fn in os.walk(src):
        for f in fn:
            if f.endswith((".h", ".hpp")):
                t = strip_code(open(os.path.join(dp, f), encoding="utf-8", errors="replace").read())
                for m in re.finditer(r"^[ \t]*(?!static\b)(?:[A-Za-z_][\w\*&]*[ \t\*&]+)+([A-Za-z_]\w*)[ \t]*\([^;{]*\)[ \t]*;",
                                     t, re.M):
                    names.add(m.group(1))
    if not names:
        return
    pat = re.compile(r"^([ \t]*)static[ \t]+((?:inline[ \t]+)?[A-Za-z_][\w\*&]*[ \t\*&]+(?:[\w\*&]+[ \t\*&]+)*)(" +
                     "|".join(re.escape(n) for n in sorted(names)) + r")([ \t]*\()", re.M)
    for dp, _, fn in os.walk(src):
        for f in fn:
            if f.endswith((".cpp", ".ino", ".c")):
                p = os.path.join(dp, f)
                t = open(p, encoding="utf-8", errors="replace").read()
                t2 = pat.sub(r"\1\2\3\4", t)
                if t2 != t:
                    open(p, "w", encoding="utf-8").write(t2)


# Source idioms that assume 16-bit AVR pointers, rewritten wherever they appear.
GENERIC_FIXES = [
    # OBONO's module tables: "(uint16_t) &moduleTable[idx] + 2" = the 2nd function pointer on AVR
    (r"#define\s+callInitFunc\(idx\)[^\n]*", "#define callInitFunc(idx)   (moduleTable[idx].initFunc())"),
    (r"#define\s+callUpdateFunc\(idx\)[^\n]*", "#define callUpdateFunc(idx) (moduleTable[idx].updateFunc())"),
    (r"#define\s+callDrawFunc\(idx\)[^\n]*", "#define callDrawFunc(idx)   (moduleTable[idx].drawFunc())"),
    # OBONO: declared with int, defined with int16_t (the same type on the AVR)
    (r"static void drawText\(const char \*p, int lines\);", "static void drawText(const char *p, int16_t lines);"),
    # waiting for an ADC conversion (random seed from a floating pin): there is no ADC to finish
    (r"while\s*\(\s*bit_is_set\s*\(\s*ADCSRA\s*,\s*ADSC\s*\)\s*\)", "while (0)"),
    (r"while\s*\(\s*\(?\s*ADCSRA\s*&\s*_BV\s*\(\s*ADSC\s*\)\s*\)?\s*\)", "while (0)"),
    # `static` repeated on an out-of-class member definition (gcc -fpermissive only)
    (r"(?m)^([ \t]*)static[ \t]+((?:[\w<>\*&]+[ \t]+)+[\*&]*\w+::~?\w+[ \t]*\()", r"\1\2"),
]
# #include "src\foo.h" (Windows paths) -> forward slashes
INCLUDE_BACKSLASH = re.compile(r'(#\s*include\s*")([^"\n]*\\[^"\n]*)(")')


def generic_fixes(src):
    for dp, _, fn in os.walk(src):
        for f in fn:
            if f.endswith((".cpp", ".ino", ".c", ".h", ".hpp")):
                p = os.path.join(dp, f)
                t = open(p, encoding="utf-8", errors="replace").read()
                t2 = t
                for pat, rep in GENERIC_FIXES:   # rep is a re.sub template
                    t2 = re.sub(pat, rep, t2)
                t2 = INCLUDE_BACKSLASH.sub(lambda m: m.group(1) + m.group(2).replace("\\", "/") + m.group(3), t2)
                # an Arduboy2Base subclass with its own global BitStreamReader: newer Arduboy2 has a
                # nested Arduboy2Base::BitStreamReader that would be found first
                if re.search(r"^struct BitStreamReader\b", t2, re.M):
                    t2 = re.sub(r"(?<![:\w])BitStreamReader(\s+\w+\s*=\s*)BitStreamReader\(", r"::BitStreamReader\1::BitStreamReader(", t2)
                    t2 = re.sub(r"(?<![:\w])BitStreamReader(\s+\w+\s*\()", r"::BitStreamReader\1", t2)
                if t2 != t:
                    open(p, "w", encoding="utf-8").write(t2)


def prepare(g):
    """Copy the sketch into gen/<slug>/src, merge its .ino files into sketch.cpp (Arduino style)."""
    base = os.path.join(SRC, "games", g["slug"], g.get("path", ""))
    if not os.path.isdir(base):
        raise RuntimeError(f"no sources at {base} (run fetch.sh)")
    out = os.path.join(GEN, g["slug"])
    src = os.path.join(out, "src")
    shutil.rmtree(src, ignore_errors=True)
    shutil.copytree(base, src, ignore=shutil.ignore_patterns(".git", "*.hex", "*.png", "*.bin", "*.gif", "*.jpg"))
    for rel in g.get("drop_files", []):                  # e.g. an old copy of the sketch next to it
        os.remove(os.path.join(src, rel))
    for rel, text in g.get("replace_files", {}).items():   # whole files written by us (BSD, see games.py)
        with open(os.path.join(src, rel), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    for rel, text in g.get("patches", {}).items():   # small per-game fixes: {file: [(old, new), ...]}
        p = os.path.join(src, rel)
        s = open(p, encoding="utf-8", errors="replace").read()
        for old, new in text:
            if old.startswith("re:"):
                s2 = re.sub(old[3:], new, s, flags=re.S)   # new is a re.sub template
                if s2 == s:
                    raise RuntimeError(f"patch pattern not found in {rel}: {old[:50]}")
                s = s2
                continue
            if old not in s:
                raise RuntimeError(f"patch target not found in {rel}: {old[:50]}")
            s = s.replace(old, new)
        open(p, "w", encoding="utf-8").write(s)
    drop_bundled_libs(src)
    fix_static_conflicts(src)
    generic_fixes(src)
    inos = sorted(f for f in os.listdir(src) if f.endswith(".ino"))
    if not inos:
        raise RuntimeError("no .ino file")
    main = os.path.basename(os.path.normpath(base)) + ".ino"
    if main not in inos:
        main = next((f for f in inos if "setup(" in open(os.path.join(src, f), errors="replace").read()), inos[0])
    order = [main] + [f for f in inos if f != main]
    parts = []
    for f in order:
        text = open(os.path.join(src, f), encoding="utf-8", errors="replace").read()
        parts.append(f'#line 1 "{f}"\n' + text + "\n")
        os.rename(os.path.join(src, f), os.path.join(src, f + ".orig"))
    code = "".join(parts)
    first, protos = find_prototypes(code)
    pp = ino_only_preprocessed(code, g, src)
    if pp is not None:
        protos = find_prototypes(pp)[1]
    protos = list(dict.fromkeys(protos))
    if protos and first is not None:
        line = code[:first].count("\n") + 1
        # the #line of the file that contains `first`
        lm = list(re.finditer(r'^#line 1 "(.+)"$', code[:first], re.M))
        fname = lm[-1].group(1) if lm else main
        fline = code[lm[-1].end():first].count("\n") if lm else line
        code = (code[:first] + "\n// prototypes (Arduino builder style)\n" + "\n".join(protos) +
                f'\n#line {fline} "{fname}"\n' + code[first:])
    with open(os.path.join(src, "sketch.cpp"), "w", encoding="utf-8") as fh:
        fh.write("#include <Arduino.h>\n" + code)
    with open(os.path.join(src, "nvab_meta.cpp"), "w", encoding="utf-8") as fh:
        fh.write('extern "C" const char nvab_title[] = %s;\n' % json.dumps(g["menu_title"]))
        fh.write('extern "C" const int nvab_blend = %d;\n' % (1 if g.get("blend") else 0))
    return out, src


def sources_of(src, extra_dirs=()):
    """What the Arduino builder compiles: the sketch folder itself and its src/ tree."""
    files = [os.path.join(src, f) for f in os.listdir(src) if f.endswith((".cpp", ".c", ".cc"))]
    for sub in ("src",) + tuple(extra_dirs):
        for dp, dn, fn in os.walk(os.path.join(src, sub)):
            for f in fn:
                if f.endswith((".cpp", ".c", ".cc")):
                    files.append(os.path.join(dp, f))
    return sorted(set(files))


def compile_all(g, src, kind, objdir):
    cxx, cc = (WASM_CXX, WASM_CC) if kind == "wasm" else (NAT_CXX, NAT_CC)
    os.makedirs(objdir, exist_ok=True)
    inc = INCLUDES + ["-I" + src] + ["-I" + os.path.join(src, d) for d in g.get("include_dirs", [])]
    defs = ["-D" + d for d in g.get("defines", [])]
    objs, errs = [], []
    for f in sources_of(src, g.get("source_dirs", ())):
        o = os.path.join(objdir, os.path.relpath(f, src).replace("/", "_") + ".o")
        if f.endswith(".c"):
            cmd = cc + inc + defs + ["-include", "Arduino.h", "-c", f, "-o", o]
        else:
            cmd = cxx + inc + defs + g.get("cxxflags", []) + ["-include", "Arduino.h", "-c", f, "-o", o]
        r = run(cmd)
        if r.returncode:
            errs.append(r.stderr)
        objs.append(o)
    return objs, errs


def first_errors(errs, n=6):
    lines = []
    for e in errs:
        for ln in e.splitlines():
            if "error" in ln:
                lines.append(ln.strip())
    return lines[:n]


def link_wasm(objs, out_wasm):
    lib = os.path.join(GEN, "lib-wasm")
    cmd = CLANGXX + ["-O2", "-nodefaultlibs", "-mexec-model=reactor", "-Wl,--export=run",
                     "-Wl,-z,stack-size=65536", "-Wl,--no-stack-first", "-Wl,--strip-all", "-Wl,--gc-sections", "-o", out_wasm,
                     os.path.join(lib, "nvab.o")] + objs + [os.path.join(lib, "libab.a"), "-lc"] + BUILTINS
    return run(cmd)


def link_native(objs, out_bin):
    lib = os.path.join(GEN, "lib-native")
    harness_o = os.path.join(lib, "harness.o")
    if not os.path.exists(harness_o) or os.path.getmtime(harness_o) < os.path.getmtime(
            os.path.join(HERE, "host", "harness.c")):
        r = run(NAT_CC + ["-I" + SDK_INC, "-I" + os.path.join(HERE, "host"), "-c",
                          os.path.join(HERE, "host", "harness.c"), "-o", harness_o])
        if r.returncode:
            return r
    return run(["g++", "-o", out_bin, harness_o, os.path.join(lib, "nvab.o")] + objs +
               [os.path.join(lib, "libab.a"), "-lm"])


# ---- harness + images ------------------------------------------------------------------------------------
def abhost_bin():
    host = os.path.join(GEN, "abhost")
    srcs = [os.path.join(HERE, "host", f) for f in ("abhost.c", "harness.c", "font5x7.h")]
    if not os.path.exists(host) or os.path.getmtime(host) < max(os.path.getmtime(p) for p in srcs):
        r = run(["gcc", "-O2", "-DNV_SIM", "-I" + SDK_INC, "-I" + WAMR_INC, "-I" + os.path.join(HERE, "host"), "-o",
                 host, os.path.join(HERE, "host", "abhost.c"), WAMR_LIB, "-lm", "-lpthread", "-ldl"])
        if r.returncode:
            sys.exit(r.stderr)
    return host


def harness_run(g, module, outdir, frames=FRAMES):
    """The real app under WAMR (abhost) with scripted input; canvas snapshots in outdir."""
    fs = os.path.join(outdir, "fs")
    shutil.rmtree(fs, ignore_errors=True)
    os.makedirs(fs)
    snaps = sorted(set([g["icon_frame"]] + list(g["shot_frames"]) +
                       [20, 35, 60, 80, 100, 150, 200, 250, 300, 450, 600, 900, frames - 1]))
    env = dict(os.environ, AB_SNAP=",".join(str(s) for s in snaps))
    return run([abhost_bin(), module, fs, str(frames), os.path.join(outdir, "final.ppm"),
                g.get("script", DEFAULT_SCRIPT_OF(g))], env=env, timeout=300)


def DEFAULT_SCRIPT_OF(g):
    return G.DEFAULT_SCRIPT


def frame_from_canvas(ppm, scale=6):
    """The 128x64 1-bit game frame, read back from a canvas snapshot (default zoom 6x, white)."""
    from PIL import Image
    im = Image.open(ppm).convert("L")
    gx, gy = (1024 - 128 * scale) // 2, (600 - 64 * scale) // 2
    out = Image.new("L", (128, 64))
    px = im.load()
    op = out.load()
    for y in range(64):
        for x in range(128):
            op[x, y] = 255 if px[gx + x * scale + scale // 2 - 1, gy + y * scale + scale // 2 - 1] > 110 else 0
    return out


def lit_ratio(pgm):
    from PIL import Image
    im = Image.open(pgm).convert("L")
    h = im.histogram()
    return sum(h[128:]) / (128 * 64)


def main_blob(fr):
    """Bounding box of the title logo: the connected group of lit pixels (letters merged by a 3 px
    dilation) holding the most pixels; a frame around the whole screen counts much less."""
    from PIL import ImageFilter
    lit = fr.point(lambda v: 255 if v > 127 else 0)
    grown = lit.filter(ImageFilter.MaxFilter(7)).load()
    src = lit.load()
    seen = [[False] * 64 for _ in range(128)]
    best, best_score = None, -1
    for sx in range(128):
        for sy in range(64):
            if seen[sx][sy] or not grown[sx, sy]:
                continue
            stack, n, bx0, by0, bx1, by1 = [(sx, sy)], 0, sx, sy, sx, sy
            seen[sx][sy] = True
            while stack:
                x, y = stack.pop()
                if src[x, y]:
                    n += 1
                bx0, by0, bx1, by1 = min(bx0, x), min(by0, y), max(bx1, x), max(by1, y)
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        nx, ny = x + dx, y + dy
                        if 0 <= nx < 128 and 0 <= ny < 64 and not seen[nx][ny] and grown[nx, ny]:
                            seen[nx][ny] = True
                            stack.append((nx, ny))
            score = n * (0.25 if (bx1 - bx0 > 118 and by1 - by0 > 54) else 1.0)
            if score > best_score:
                best, best_score = (bx0, by0, bx1 + 1, by1 + 1), score
    return best


def make_icon(g, pgm, out):
    """80x80 LVGL ARGB8888 (B,G,R,A) raw-deflate icon: the game's own title frame on a tile."""
    from PIL import Image, ImageDraw
    S = 4
    fr = Image.open(pgm).convert("L")
    bbox = (main_blob(fr) if not g.get("icon_full") else None) or \
        fr.point(lambda v: 255 if v > 127 else 0).getbbox() or (0, 0, 128, 64)
    x0, y0, x1, y1 = bbox
    crop = g.get("icon_crop")
    if crop:
        x0, y0, x1, y1 = crop
    else:
        pad = 2
        x0, y0, x1, y1 = max(0, x0 - pad), max(0, y0 - pad), min(128, x1 + pad), min(64, y1 + pad)
        w, h = x1 - x0, y1 - y0
        if w < h * 1.0:   # widen very tall crops a bit
            extra = int(h * 1.0) - w
            x0, x1 = max(0, x0 - extra // 2), min(128, x1 + extra - extra // 2)
    pic = fr.crop((x0, y0, x1, y1))
    fg = tuple(int(g.get("icon_fg", "#f2f5ff")[i:i + 2], 16) for i in (1, 3, 5))
    bg = tuple(int(g.get("icon_bg", "#1b1e27")[i:i + 2], 16) for i in (1, 3, 5))
    rim = tuple(int(g.get("icon_rim", "#3a4150")[i:i + 2], 16) for i in (1, 3, 5))
    tile = Image.new("RGBA", (80 * S, 80 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(tile)
    d.rounded_rectangle((2 * S, 2 * S, 78 * S, 78 * S), radius=18 * S, fill=rim + (255,))
    d.rounded_rectangle((5 * S, 5 * S, 75 * S, 75 * S), radius=15 * S, fill=bg + (255,))
    box = 64 * S
    pw, ph = pic.size
    k = min(box / pw, box / ph)
    # nearest-neighbour at 4x supersampling keeps the pixel art sharp after the final downscale
    nw, nh = max(1, int(pw * k)), max(1, int(ph * k))
    big = pic.resize((nw, nh), Image.NEAREST)
    colored = Image.new("RGBA", big.size, fg + (255,))
    tile.paste(colored, ((80 * S - nw) // 2, (80 * S - nh) // 2), big)
    tile = tile.resize((80, 80), Image.LANCZOS)
    raw = tile.tobytes("raw", "BGRA")
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(out, "wb").write(co.compress(raw) + co.flush())
    tile.save(out[:-2] + ".png") if g.get("keep_icon_png") else None
    return tile


def make_shot(ppm, out):
    from PIL import Image
    im = Image.open(ppm).convert("RGB").resize((512, 300), Image.LANCZOS)
    im.save(out, "JPEG", quality=90, optimize=False, progressive=False)


# ---- manifest + guides ----------------------------------------------------------------------------------
def write_manifest(g, d):
    lic = g["license"]
    m = {
        "id": "ab-" + g["slug"],
        "name": g["title"],
        "version": "1.0.0",
        "entry": "run",
        "abi": 11,
        "ram_budget": 3145728,
        "stack_kb": 64,
        "timeout_ms": 120000,
        "permissions": ["gfx", "fs", "log"],
        "canvas_w": 1024,
        "canvas_h": 600,
        "system_gestures": False,
        "category": "games",
        "author": g["author"] + "; Arduboy2 library by Scott Allen and contributors",
        "license": f"{lic} (game); BSD-3-Clause (Arduboy2 library and NucleoOS shim)",
        "source": g["source"],
        "description": g["desc_en"],
        "descriptions": {"en": g["desc_en"], "it": g["desc_it"]},
    }
    with open(os.path.join(d, "manifest.json"), "w", encoding="utf-8") as fh:
        json.dump(m, fh, indent=2, ensure_ascii=False)
        fh.write("\n")


def license_text(g):
    base = os.path.join(SRC, "games", g["slug"])
    p = os.path.join(base, g["license_file"])
    return open(p, encoding="utf-8", errors="replace").read().strip()


GUIDE_IT = """# {title}

{desc_it}

## Comandi

{controls_it}

- **Touch**: croce direzionale a sinistra, **A** e **B** a destra (A in basso, B in alto, come sull'Arduboy). Più dita insieme funzionano.
- **Gamepad** USB o Bluetooth: croce o levetta sinistra; A, X e Start = A; B e Y = B.
- **Tastiera USB**: frecce o WASD; Spazio/X/Invio = A; Z/C/Backspace = B.
- **MENU** (in alto a sinistra), i dorsali L/R, Select o il tasto Guide del gamepad mettono in pausa.

## Il menu di pausa

**Continua**, **Zoom** 5x, 6x o 7x (a 7x lo schermo è più grande e i comandi touch sono nascosti: comodo con gamepad o tastiera), **Colori** (bianco, ambra, verde, blu, LCD), **Audio** acceso o spento, **Esci**. Zoom, colori e audio restano memorizzati. Il gesto indietro del sistema apre il menu; ripetuto, esce.

## Salvataggi

La EEPROM dell'Arduboy (1 KB, dove i giochi tengono record e progressi) è salvata nella cartella dell'app poco dopo ogni scrittura e all'uscita.

## Crediti e licenze

**{title}** è di {author} ({license}). Sorgente: {source} (commit `{commit_short}`).{notes_it}

Gira sulla libreria Arduboy2 di Scott Allen e collaboratori (licenza BSD a 3 clausole), adattata a NucleoOS; i nomi delle note musicali vengono da ArduboyTones (MIT). Arduboy è un marchio di Arduboy Inc.; questo port non è affiliato.

### Licenza del gioco

```
{license_text}
```

### Arduboy2

```
{ab2_license}
```
"""

GUIDE_EN = """# {title}

{desc_en}

## Controls

{controls_en}

- **Touch**: D-pad on the left, **A** and **B** on the right (A lower, B upper, as on the Arduboy). Several fingers at once work.
- **Gamepad** (USB or Bluetooth): D-pad or left stick; A, X and Start = A; B and Y = B.
- **USB keyboard**: arrows or WASD; Space/X/Enter = A; Z/C/Backspace = B.
- **MENU** (top left), the L/R shoulders, Select or the pad's Guide button pause the game.

## The pause menu

**Resume**, **Zoom** 5x, 6x or 7x (7x is the biggest picture and hides the touch controls: best with a gamepad or keyboard), **Colours** (white, amber, green, blue, LCD), **Sound** on or off, **Exit**. Zoom, colours and sound are remembered. The system back gesture opens the menu; a second one exits.

## Saving

The Arduboy EEPROM (1 KB, where games keep high scores and progress) is saved in the app's folder shortly after every write and when the app closes.

## Credits and licenses

**{title}** is by {author} ({license}). Source: {source} (commit `{commit_short}`).{notes_en}

Runs on the Arduboy2 library by Scott Allen and contributors (BSD 3-clause license), adapted to NucleoOS; musical note names come from ArduboyTones (MIT). Arduboy is a trademark of Arduboy Inc.; this port is not affiliated.

### Game license

```
{license_text}
```

### Arduboy2

```
{ab2_license}
```
"""


def ab2_license():
    s = open(os.path.join(SRC, "Arduboy2", "LICENSE.txt"), encoding="utf-8").read()
    # the BSD-3 section (Arduboy2 + Arduboy + Adafruit notices) is what the binary contains
    a = s.index("Licensed under the BSD 3-clause license:")
    b = s.index("Licensed under the GNU LGPL license:")
    return s[a:b].strip().rstrip("-").strip()


NUCLEO_REPO = "https://github.com/indecenti/NucleoOS-P4"
OFFER_IT = ("\n\n**Codice sorgente ({license}).** Questo gioco è software libero: puoi ridistribuirlo e modificarlo "
            "secondo i termini della licenza riportata qui sotto (copiata anche nel file `LICENSE` dell'app). Il "
            "sorgente completo corrispondente è il repository {repo_url} al commit `{commit}`; le modifiche per "
            "NucleoOS (lo strato di compatibilità Arduboy2 e le piccole correzioni di compilazione) sono in "
            "{nucleo}/tree/main/ports/arduboy.")
OFFER_EN = ("\n\n**Source code ({license}).** This game is free software: you can redistribute and modify it under "
            "the terms of the license below (also shipped as the app's `LICENSE` file). The complete corresponding "
            "source is the repository {repo_url} at commit `{commit}`; the NucleoOS changes (the Arduboy2 "
            "compatibility layer and the small build fixes) are in {nucleo}/tree/main/ports/arduboy.")


def write_guides(g, d):
    commit = G.commit_of(g["slug"])
    notes_it = ("\n\n" + g["notes_it"]) if g.get("notes_it") else ""
    notes_en = ("\n\n" + g["notes_en"]) if g.get("notes_en") else ""
    if "GPL" in g["license"]:   # GPL/LGPL: written source offer (exact repository + commit)
        o = dict(license=g["license"], repo_url=f"https://github.com/{g['repo']}", commit=commit, nucleo=NUCLEO_REPO)
        notes_it += OFFER_IT.format(**o)
        notes_en += OFFER_EN.format(**o)
    ctx = dict(g, commit_short=commit[:10], license_text=license_text(g), ab2_license=ab2_license(),
               notes_it=notes_it, notes_en=notes_en)
    with open(os.path.join(d, "LICENSE"), "w", encoding="utf-8", newline="\n") as fh:   # the game's own license file
        fh.write(license_text(g) + "\n")
    open(os.path.join(d, "GUIDE.md"), "w", encoding="utf-8").write(GUIDE_IT.format(**ctx))
    open(os.path.join(d, "GUIDE.en.md"), "w", encoding="utf-8").write(GUIDE_EN.format(**ctx))


# ---- per game ------------------------------------------------------------------------------------------------
def aot(wasm, out):
    r = run([WAMRC, "--target=riscv32", "--target-abi=ilp32f", "--cpu=generic-rv32",
             "--cpu-features=+m,+a,+c,+f", "--enable-multi-thread", "-o", out, wasm])
    return r


def x64_aot(wasm, out):
    return run([WAMRC, "--target=x86_64", "--bounds-checks=1", "--enable-multi-thread", "-o", out, wasm])


def build_one(g, write_app=True):
    slug = g["slug"]
    res = {"slug": slug, "ok": False}
    try:
        out, src = prepare(g)
    except Exception as e:  # noqa: BLE001
        res["error"] = f"prepare: {e}"
        return res
    objs, errs = compile_all(g, src, "wasm", os.path.join(out, "obj-wasm"))
    if errs:
        res["error"] = "compile: " + " | ".join(first_errors(errs))
        return res
    wasm = os.path.join(out, "app.wasm")
    r = link_wasm(objs, wasm)
    if r.returncode:
        res["error"] = "link: " + " | ".join(ln for ln in r.stderr.splitlines() if "undefined" in ln or "error" in ln)[:600]
        return res
    res["wasm"] = os.path.getsize(wasm)
    x64 = os.path.join(out, "app.x64.aot")
    r = x64_aot(wasm, x64)
    if r.returncode:
        res["error"] = "wamrc x86_64: " + (r.stdout + r.stderr)[-300:]
        return res
    test = os.path.join(out, "test")
    shutil.rmtree(test, ignore_errors=True)
    os.makedirs(test)
    try:
        r = harness_run(g, x64, test)
    except subprocess.TimeoutExpired:
        res["error"] = "harness timeout"
        return res
    lines = r.stdout.strip().splitlines()
    res["harness"] = lines[-1] if lines else ""
    if r.returncode or "HUNG" in r.stdout or "TRAP" in r.stdout:
        bad = [ln for ln in lines if "HUNG" in ln or "TRAP" in ln or "exit code" in ln]
        res["error"] = f"harness rc={r.returncode}: " + " ".join(bad[-2:] or lines[-2:]) + r.stderr[-300:]
        return res
    snaps = sorted(glob.glob(os.path.join(test, "final_*.ppm")), key=lambda p: int(re.findall(r"_(\d+)\.ppm", p)[0]))
    lit = []
    for p in snaps:
        fr = frame_from_canvas(p)
        fr.save(p[:-4] + ".pgm")
        lit.append(round(sum(fr.histogram()[128:]) / (128 * 64), 3))
    res["lit"] = lit
    res["frames"] = sum(1 for ln in lines if ln.startswith("present"))
    pgm = os.path.join(test, f"final_{g['icon_frame']}.pgm")
    if not os.path.exists(pgm):
        res["error"] = "no frame captured (the game ended early?)"
        return res
    if max(lit or [0]) == 0:
        res["error"] = "blank screen in every snapshot"
        return res
    if not write_app:
        res["ok"] = True
        return res
    app = os.path.join(ROOT, "apps", "ab-" + slug)
    os.makedirs(os.path.join(app, "shots"), exist_ok=True)
    shutil.copy(wasm, os.path.join(app, "app.wasm"))
    r = aot(wasm, os.path.join(app, "app.aot"))
    if r.returncode:
        res["error"] = "wamrc riscv32: " + (r.stdout + r.stderr)[-400:]
        return res
    res["aot"] = os.path.getsize(os.path.join(app, "app.aot"))
    if res["wasm"] > WASM_CAP or res["aot"] > AOT_CAP:
        res["error"] = f"too big: wasm {res['wasm']} aot {res['aot']}"
        return res
    make_icon(g, pgm, os.path.join(app, "icon.z"))
    for i, f in enumerate(g["shot_frames"][:2]):
        make_shot(os.path.join(test, f"final_{f}.ppm"), os.path.join(app, "shots", f"{i + 1}.jpg"))
    write_manifest(g, app)
    write_guides(g, app)
    res["ok"] = True
    return res


def selected(args):
    if args:
        by = {g["slug"]: g for g in G.GAMES}
        missing = [a for a in args if a not in by]
        if missing:
            sys.exit(f"unknown: {missing}")
        return [G.full(by[a]) for a in args]
    return [g for g in (G.full(x) for x in G.GAMES) if not g.get("skip")]


def cmd_build(args, write_app=True):
    games = selected(args)
    r = run(["bash", os.path.join(HERE, "fetch.sh")] + [g["slug"] for g in games])
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    abhost_bin()
    results = []
    with cf.ThreadPoolExecutor(max_workers=int(os.environ.get("JOBS", "6"))) as ex:
        def safe(g):
            try:
                return build_one(g, write_app)
            except Exception as e:  # noqa: BLE001
                return {"slug": g["slug"], "ok": False, "error": f"{type(e).__name__}: {e}"}
        for res in ex.map(safe, games):
            results.append(res)
            if res["ok"]:
                log(f"OK   ab-{res['slug']:<24} wasm {res.get('wasm', '-')!s:>8} aot {res.get('aot', '-')!s:>8}  "
                    f"lit {res.get('lit')}  | {res.get('harness', '')[:110]}")
            else:
                log(f"FAIL ab-{res['slug']:<24} {res.get('error', '')[:400]}")
    json.dump(results, open(os.path.join(GEN, "last_build.json"), "w"), indent=1)
    log(f"{sum(r['ok'] for r in results)}/{len(results)} ok")


# ---- WAMR ------------------------------------------------------------------------------------------------
def cmd_wamr(args):
    """The installed app.wasm under the WAMR interpreter (slow, 300 presents) and as x86-64 AOT."""
    for g in selected(args):
        app = os.path.join(ROOT, "apps", "ab-" + g["slug"])
        test = os.path.join(GEN, g["slug"], "wamr")
        shutil.rmtree(test, ignore_errors=True)
        os.makedirs(test)
        r = harness_run(g, os.path.join(app, "app.wasm"), test, frames=300)
        log(f"== ab-{g['slug']} interp (300): rc={r.returncode} " + (r.stdout.strip().splitlines() or [""])[-1][:160])
        x64 = os.path.join(test, "app.x64.aot")
        if x64_aot(os.path.join(app, "app.wasm"), x64).returncode:
            log("   wamrc x86_64 failed")
            continue
        r = harness_run(g, x64, test)
        log(f"== ab-{g['slug']} aot  ({FRAMES}): rc={r.returncode} " + (r.stdout.strip().splitlines() or [""])[-1][:160])


def cmd_native(args):
    """Debug build: the sketch + shim compiled for x86-64 against harness.c (gdb-friendly, -g).
    Note: AVR-style (uint16_t) pointer casts only work in the wasm32 build."""
    for g in selected(args):
        out, src = prepare(g)
        objs, errs = compile_all(g, src, "native", os.path.join(out, "obj-native"))
        if errs:
            log(f"FAIL {g['slug']}: " + " | ".join(first_errors(errs)))
            continue
        binary = os.path.join(out, "harness")
        r = link_native(objs, binary)
        log(f"{g['slug']}: {binary}" if not r.returncode else r.stderr[-500:])


# ---- pinning + docs ---------------------------------------------------------------------------------------------
def cmd_pin(args):
    slug = args[0]
    g = next(x for x in G.GAMES if x["slug"] == slug)
    repo = g["repo"]
    commit = args[1] if len(args) > 1 else ""
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        ref = commit or "HEAD"
        r = run(["git", "ls-remote", f"https://github.com/{repo}.git", ref])
        if r.returncode or not r.stdout.strip():
            sys.exit(f"ls-remote {repo} {ref} failed: {r.stderr}")
        commit = r.stdout.split()[0]
    tgz = os.path.join(SRC, "tar", repo.replace("/", "_") + "-" + commit + ".tar.gz")
    if not os.path.exists(tgz):
        r = run(["curl", "-sSfL", "-o", tgz, f"https://codeload.github.com/{repo}/tar.gz/{commit}"])
        if r.returncode:
            sys.exit(f"download failed: {r.stderr}")
    sha = hashlib.sha256(open(tgz, "rb").read()).hexdigest()
    fetch = os.path.join(HERE, "fetch.sh")
    s = open(fetch, encoding="utf-8").read()
    line = f"game {slug} {repo} {commit} \\\n    {sha}\n"
    s = re.sub(r"^game " + re.escape(slug) + r" .*?\n.*?\n", "", s, flags=re.M)
    s = s.replace("# GAMES-END\n", line + "# GAMES-END\n")
    open(fetch, "w", encoding="utf-8").write(s)
    log(f"pinned {slug}: {repo}@{commit} {sha}")


def cmd_sheet(args):
    """Contact sheet of the harness frames (one row per game) -> gen/sheet.png, for a visual check."""
    from PIL import Image, ImageDraw
    games = selected(args)
    rows = []
    for g in games:
        pgms = sorted(glob.glob(os.path.join(GEN, g["slug"], "test", "final_*.pgm")),
                      key=lambda p: int(re.findall(r"_(\d+)\.pgm", p)[0]))
        if pgms:
            rows.append((g["slug"], pgms))
    if not rows:
        sys.exit("nothing to show")
    n = max(len(p) for _, p in rows)
    W, H, L = 2 * 128 + 6, 2 * 64 + 6, 150
    sheet = Image.new("RGB", (L + n * W, len(rows) * H), (40, 40, 48))
    d = ImageDraw.Draw(sheet)
    for r, (slug, pgms) in enumerate(rows):
        d.text((4, r * H + 4), slug, fill=(230, 230, 230))
        for c, p in enumerate(pgms):
            im = Image.open(p).convert("RGB").resize((256, 128), Image.NEAREST)
            sheet.paste(im, (L + c * W, r * H + 3))
            d.text((L + c * W + 2, r * H + 3), re.findall(r"_(\d+)\.pgm", p)[0], fill=(255, 80, 80))
    out = os.path.join(GEN, "sheet.png")
    sheet.save(out)
    log(out)


def cmd_icons(args):
    """Redo icon.z / shots / manifest / guides of built apps from the last harness run (no rebuild)."""
    for g in selected(args):
        app = os.path.join(ROOT, "apps", "ab-" + g["slug"])
        test = os.path.join(GEN, g["slug"], "test")
        pgm = os.path.join(test, f"final_{g['icon_frame']}.pgm")
        if not os.path.isdir(app) or not os.path.exists(pgm):
            log(f"skip {g['slug']}: not built")
            continue
        make_icon(g, pgm, os.path.join(app, "icon.z"))
        for i, f in enumerate(g["shot_frames"][:2]):
            make_shot(os.path.join(test, f"final_{f}.ppm"), os.path.join(app, "shots", f"{i + 1}.jpg"))
        write_manifest(g, app)
        write_guides(g, app)
    log("icons, shots, manifests and guides rewritten")


def cmd_docs():
    G.write_docs(ROOT, HERE)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    c, args = sys.argv[1], sys.argv[2:]
    if c == "libs":
        cmd_libs()
    elif c == "build":
        cmd_build(args)
    elif c == "test":
        cmd_build(args, write_app=False)
    elif c == "wamr":
        cmd_wamr(args)
    elif c == "native":
        cmd_native(args)
    elif c == "pin":
        cmd_pin(args)
    elif c == "icons":
        cmd_icons(args)
    elif c == "sheet":
        cmd_sheet(args)
    elif c == "docs":
        cmd_docs()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
