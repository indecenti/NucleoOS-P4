"""patch_sources.py — copy the pinned doomgeneric + Chocolate Doom sources into a build folder and
apply the NucleoOS edits (small, anchored text replacements; each must match exactly once).

    python ports/doom/patch_sources.py <ports/_src/doom> <out-dir>

Edits:
  g_game.c      analog sticks: DG_AnalogTiccmd() adds the pad's walk/strafe/turn to each tic
  m_config.c    config + saves in the app's private folder, one save folder per IWAD
  i_oplmusic.c  Chocolate's player against doomgeneric's older headers
"""
import os
import shutil
import sys

src, out = sys.argv[1], sys.argv[2]
dg = os.path.join(src, "doomgeneric")
os.makedirs(out, exist_ok=True)

SKIP = ("doomgeneric_", "i_sdl", "i_allegro")   # other platforms' back ends
for name in os.listdir(dg):
    if name.endswith((".c", ".h")) and not name.startswith(SKIP):
        shutil.copyfile(os.path.join(dg, name), os.path.join(out, name))
for name in ("opl.h", "opl_queue.c", "opl_queue.h", "i_oplmusic.c", "midifile.c", "midifile.h"):
    shutil.copyfile(os.path.join(src, "choco", name), os.path.join(out, name))
for name in ("emu8950.c", "emu8950.h", "slot_render.h"):
    shutil.copyfile(os.path.join(src, "emu8950", name), os.path.join(out, name))


def edit(name, old, new, every=False):
    p = os.path.join(out, name)
    raw = open(p, "rb").read()
    crlf = b"\r\n" in raw
    s = raw.decode("latin-1").replace("\r\n", "\n")
    n = s.count(old)
    if n != 1 and not (every and n):
        sys.exit(f"patch_sources: {name}: anchor found {n} times:\n{old}")
    s = s.replace(old, new)
    if crlf:
        s = s.replace("\n", "\r\n")
    open(p, "wb").write(s.encode("latin-1"))


edit("g_game.c", '#include "g_game.h"\n',
     '#include "g_game.h"\n\nvoid DG_AnalogTiccmd(int *forward, int *side, short *angleturn);\n')
edit("g_game.c", "    forward += mousey; \n",
     "    forward += mousey; \n\n"
     "    // NucleoOS: analog sticks (ports/doom/nv_doom.c)\n"
     "    DG_AnalogTiccmd(&forward, &side, &cmd->angleturn);\n")

edit("m_config.c", """static char *GetDefaultConfigDir(void)
{
    char *result = (char *)malloc(2);
    result[0] = '.';
    result[1] = '\\0';

    return result;
}""", """extern const char *nv_doom_datadir;   // NucleoOS: the app's private folder

static char *GetDefaultConfigDir(void)
{
    return M_StringDuplicate(nv_doom_datadir);
}""")
edit("m_config.c",
     'savegamedir = M_StringJoin(configdir, DIR_SEPARATOR_S, ".savegame/", NULL);',
     'savegamedir = M_StringJoin(configdir, "save-", iwadname, DIR_SEPARATOR_S, NULL);')
# Chocolate's OPL player + MIDI parser against doomgeneric's older headers
edit("doomtype.h", "#define PACKEDATTR __attribute__((packed))\n",
     "#define PACKEDATTR __attribute__((packed))\n#define PACKED_STRUCT(...) struct __VA_ARGS__ PACKEDATTR\n")
edit("i_sound.h", "extern music_module_t music_opl_module;\n",
     "extern const music_module_t music_opl_module;\n\n"
     "// DMX version to emulate for OPL emulation (Chocolate Doom i_sound.h):\n"
     "typedef enum {\n    opl_doom1_1_666,\n    opl_doom2_1_666,\n    opl_doom_1_9\n} opl_driver_ver_t;\n\n"
     "void I_SetOPLDriverVer(opl_driver_ver_t ver);\nvoid I_OPL_DevMessages(char *, size_t);\n")
edit("i_sound.c", "#if defined(FEATURE_SOUND) && !defined(__DJGPP__)\n#include <SDL_mixer.h>\n#endif\n", "")
with open(os.path.join(out, "m_misc.c"), "ab") as f:   # Chocolate helpers the OPL player uses
    f.write(b"\nFILE *M_fopen(const char *f, const char *m) { return fopen(f, m); }\n"
            b"int M_remove(const char *p) { return remove(p); }\n")
edit("midifile.c", '#include "midifile.h"\n',
     '#include "midifile.h"\n\n#define SDL_SwapBE32(x) __builtin_bswap32(x)\n#define SDL_SwapBE16(x) __builtin_bswap16(x)\n')
edit("m_misc.h", '#include "doomtype.h"\n',
     '#include "doomtype.h"\n\nFILE *M_fopen(const char *filename, const char *mode);\nint M_remove(const char *path);\n')
edit("i_system.h", '#include "d_event.h"\n',
     '#include "d_event.h"\n\n#include <stddef.h>\nvoid *I_Realloc(void *ptr, size_t size);\n')
# temp files (the OPL player round-trips each song through one): the app's own folder, not /tmp
edit("m_misc.c", '    tempdir = "/tmp";\n',
     '    extern const char *nv_doom_datadir;\n    tempdir = (char *)nv_doom_datadir;\n')
# emu8950 (rp2040-doom): a debugging hook left in the C block renderer that only links on the Pico
edit("emu8950.c", "            if (hack_ch == 0 && s == 12) {\n                breako();\n            }\n", "", every=True)
# Limits: the vanilla static tables sized for 1994 PCs overflow (I_Error) on big community maps.
# Bigger tables cost a few hundred KB and keep vanilla behaviour below the old limits.
edit("r_plane.c", "#define MAXVISPLANES\t128", "#define MAXVISPLANES\t512")
edit("r_plane.c", "#define MAXOPENINGS\tSCREENWIDTH*64", "#define MAXOPENINGS\tSCREENWIDTH*256")
edit("r_defs.h", "#define MAXDRAWSEGS\t\t256", "#define MAXDRAWSEGS\t\t2048")
edit("r_things.h", "#define MAXVISSPRITES  \t128", "#define MAXVISSPRITES  \t1024")
edit("r_bsp.c", "#define MAXSEGS\t\t32", "#define MAXSEGS\t\t256")
edit("p_spec.h", "#define MAXBUTTONS\t\t16", "#define MAXBUTTONS\t\t128")
edit("p_spec.h", "#define MAXPLATS\t\t30", "#define MAXPLATS\t\t512")
edit("p_spec.h", "#define MAXCEILINGS\t\t30", "#define MAXCEILINGS\t\t512")
edit("p_spec.c", "#define MAXLINEANIMS            64", "#define MAXLINEANIMS            4096")
# zone: 8 MB when the app's memory allows it, down to 4 MB otherwise (AutoAllocMemory steps down)
edit("i_system.c", "#define DEFAULT_RAM 6 /* MiB */", "#define DEFAULT_RAM 8 /* MiB */")
edit("i_system.c", "#define MIN_RAM     6  /* MiB */", "#define MIN_RAM     4  /* MiB */")
# Missing textures/flats: a PWAD that replaces TEXTURE1 (made for the commercial IWAD) must not
# kill the whole game on the base IWAD's other levels. Warn and draw the first texture/flat.
edit("r_data.c", """    if (i==-1)
    {
\tI_Error ("R_TextureNumForName: %s not found",
\t\t name);
    }
    return i;""", """    if (i==-1)
    {
\tprintf ("R_TextureNumForName: %.8s not found, using a placeholder\\n", name);
\treturn 1;   // texture 0 is the "no texture" marker
    }
    return i;""")
edit("r_data.c", """\tI_Error ("R_FlatNumForName: %s not found",namet);
    }
    return i - firstflat;""", """\tprintf ("R_FlatNumForName: %s not found, using a placeholder\\n",namet);
\treturn 0;
    }
    if (i < firstflat || i > lastflat)   // a same-named non-flat lump: not usable as a flat
\treturn 0;
    return i - firstflat;""")
# AOT: WAMR's riscv32 (ilp32f) relocations lack __fixunssfdi/__fixsfdi (float -> 64-bit int), so
# an app.aot converting a float straight to uint64 is rejected at load. Go through double.
edit("opl_queue.c", "queue->entries[i].time = time + (uint64_t) (offset / factor);",
     "queue->entries[i].time = time + (uint64_t) ((double) offset / factor);")
# Long loads (startup: every texture of a 28 MB IWAD; level loads) run seconds without a frame
# and the OS wedge watchdog kills a game that presents nothing for 8 s. Every lump read gives the
# front-end a chance to present (it does so only when the game loop has been silent > 0.5 s).
edit("w_wad.c", """void W_ReadLump(unsigned int lump, void *dest)
{
    int c;
    lumpinfo_t *l;
""", """void W_ReadLump(unsigned int lump, void *dest)
{
    int c;
    lumpinfo_t *l;
    extern void DG_Pulse(void);

    DG_Pulse();
""")
# WebAssembly checks call_indirect signatures: G_CheckDemoStatus returns boolean but was cast to
# atexit_func_t (void (*)(void)), so quitting trapped with "indirect call type mismatch".
edit("d_main.c", "    I_AtExit((atexit_func_t) G_CheckDemoStatus, true);",
     "    I_AtExit(DG_CheckDemoStatusAtExit, true);")
edit("d_main.c", "void D_DoomMain (void)\n{", "void DG_CheckDemoStatusAtExit(void) { G_CheckDemoStatus(); }\n\nvoid D_DoomMain (void)\n{")
# Fatal errors: stderr goes nowhere visible on the device. Hand the message to the front-end,
# which logs it and shows it until the user dismisses it (nv_doom.c DG_FatalError).
edit("i_system.c", """    M_vsnprintf(msgbuf, sizeof(msgbuf), error, argptr);
    va_end(argptr);
""", """    M_vsnprintf(msgbuf, sizeof(msgbuf), error, argptr);
    va_end(argptr);
    {
        extern void DG_FatalError(const char *msg);
        DG_FatalError(msgbuf);
    }
""")
print("patched sources in", out)
