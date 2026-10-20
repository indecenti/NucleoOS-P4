"""lua_table.py WORDGRINDER_DIR LUAU_BC OUT.h — WordGrinder's Lua sources (the list in
src/lua/build.py, same order) compiled to Luau bytecode with luau_bc and written as the C++ table
src/c/lua.cc loads (script_table, built with -DNV_PRECOMPILED). Chunk names stay the source
paths, so error messages still name the file."""
import os
import re
import subprocess
import sys
import tempfile

g, lbc, out = sys.argv[1:4]
text = open(os.path.join(g, "src/lua/build.py"), encoding="utf-8").read()
srcs = re.findall(r'"(src/lua/[^"]+\.lua)"', text.split("SRCS = [", 1)[1].split("]", 1)[0])
parts, rows, total, total_src = [], [], 0, 0
with tempfile.TemporaryDirectory() as tmp:
    for i, src in enumerate(srcs):
        bc = os.path.join(tmp, f"{i}.bc")
        subprocess.run([lbc, os.path.join(g, src), bc], check=True)
        data = open(bc, "rb").read()
        total += len(data)
        total_src += os.path.getsize(os.path.join(g, src))
        parts.append(f"static const uint8_t file_{i}[] = {{" + ",".join(str(b) for b in data) + "};\n")
        rows.append(f'  {{ std::string((const char*) file_{i}, sizeof(file_{i})), "{src}" }},\n')
with open(out, "w", encoding="utf-8") as f:
    f.write("/* Luau bytecode of WordGrinder's Lua sources (ports/tui/wordgrinder/lua_table.py) */\n")
    f.writelines(parts)
    f.write("const FileDescriptor script_table[] = {\n")
    f.writelines(rows)
    f.write("  {}\n};\n")
print(f"  lua: {len(srcs)} files, {total_src} bytes of source -> {total} bytes of bytecode")
