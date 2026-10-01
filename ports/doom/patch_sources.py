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
# DeHackEd: Chocolate's loader replaces doomgeneric's header-only stubs
for name in os.listdir(os.path.join(src, "choco", "deh")):
    shutil.copyfile(os.path.join(src, "choco", "deh", name), os.path.join(out, name))
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
     "#define PACKEDATTR __attribute__((packed))\n#define PACKED_STRUCT(...) struct __VA_ARGS__ PACKEDATTR\n"
     "#ifndef PRINTF_ATTR\n#define PRINTF_ATTR(fmt, first) __attribute__((format(printf, fmt, first)))\n"
     "#define PRINTF_ARG_ATTR(x) __attribute__((format_arg(x)))\n#endif\n"
     "#ifndef NORETURN\n#define NORETURN __attribute__((noreturn))\n#endif\n")
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
# Limit-removing maps: more than 32767 sidedefs / vertexes (NOVA MAP30: 44k sidedefs) wrapped
# negative through the signed map fields. Read them unsigned, like every limit-removing port;
# 0xFFFF stays "no side".
edit("r_defs.h", "    short\tsidenum[2];", "    int\tsidenum[2];")
edit("p_setup.c", "\tld->sidenum[0] = SHORT(mld->sidenum[0]);\n\tld->sidenum[1] = SHORT(mld->sidenum[1]);",
     "\tld->sidenum[0] = (unsigned short) SHORT(mld->sidenum[0]);\n"
     "\tld->sidenum[1] = (unsigned short) SHORT(mld->sidenum[1]);\n"
     "\tif (ld->sidenum[0] == 0xFFFF) ld->sidenum[0] = -1;\n"
     "\tif (ld->sidenum[1] == 0xFFFF) ld->sidenum[1] = -1;")
edit("p_setup.c", "\tli->v1 = &vertexes[SHORT(ml->v1)];\n\tli->v2 = &vertexes[SHORT(ml->v2)];",
     "\tli->v1 = &vertexes[(unsigned short) SHORT(ml->v1)];\n\tli->v2 = &vertexes[(unsigned short) SHORT(ml->v2)];")
edit("p_setup.c", "\tlinedef = SHORT(ml->linedef);", "\tlinedef = (unsigned short) SHORT(ml->linedef);")
edit("p_setup.c", "\tv1 = ld->v1 = &vertexes[SHORT(mld->v1)];\n\tv2 = ld->v2 = &vertexes[SHORT(mld->v2)];",
     "\tv1 = ld->v1 = &vertexes[(unsigned short) SHORT(mld->v1)];\n"
     "\tv2 = ld->v2 = &vertexes[(unsigned short) SHORT(mld->v2)];")
# A sector special the engine doesn't know (Boom/ZDoom maps) must not end the game.
edit("p_spec.c", """      default:
\tI_Error ("P_PlayerInSpecialSector: "
\t\t "unknown special %i",
\t\t sector->special);
\tbreak;""", """      default:
\tbreak;   // NucleoOS: unknown (Boom and later) sector specials are ignored""")
# DeHackEd on (Chocolate's loader is copied in above), and DEHACKED lumps inside the PWADs are
# applied automatically (Chocolate asks for -dehlump; a store game can't pass switches by hand).
edit("doomfeatures.h", "#undef FEATURE_DEHACKED", "#define FEATURE_DEHACKED")
# PWADs with their own sprites / flats (new monsters, animated liquids) need the DeuTex-style
# merge of those namespaces (Chocolate's -merge, w_merge.c fetched with the DEH files): the
# front-end loads every PWAD with -merge.
edit("doomfeatures.h", "#undef FEATURE_WAD_MERGE", "#define FEATURE_WAD_MERGE")
# Chocolate's loader against doomgeneric: lumpinfo is an array of structs here, and the
# autoload-a-directory feature (glob) has no use on the device.
edit("deh_io.c", "lumpinfo[lumpnum]->name", "lumpinfo[lumpnum].name")
edit("deh_main.c", '#include "i_glob.h"\n', "")
# Like the source ports these patches were made for: long replacement strings are fine in files
# too (not only in lumps), and a line the parser rejects is a warning, not the end of the game.
edit("deh_main.c", """    deh_allow_long_strings = false;
    deh_allow_long_cheats = false;
    deh_allow_extended_strings = false;

    printf(" loading %s\\n", filename);""", """    deh_allow_long_strings = true;
    deh_allow_long_cheats = true;
    deh_allow_extended_strings = false;

    printf(" loading %s\\n", filename);""")
edit("deh_main.c", '        I_Error("Error parsing dehacked file");', '        printf("DEH: errors in %s, the rest of the patch is applied\\n", filename);')
edit("deh_main.c", '        I_Error("Error parsing dehacked lump");', '        printf("DEH: errors in a DEHACKED lump, the rest of the patch is applied\\n");')
# doomgeneric's D_TryFindWADByName returns its argument when the path exists (newer Chocolate
# returns a copy): only free a copy.
edit("deh_main.c", """            DEH_LoadFile(filename);
            free(filename);""", """            DEH_LoadFile(filename);
            if (filename != myargv[p]) free(filename);""")
edit("deh_main.c", """    const char *filename;
    glob_t *glob;

    glob = I_StartMultiGlob(path, GLOB_FLAG_NOCASE|GLOB_FLAG_SORTED,
                            "*.deh", "*.hhe", "*.seh", NULL);
    for (;;)
    {
        filename = I_NextGlob(glob);
        if (filename == NULL)
        {
            break;
        }
        printf(" [autoload]");
        DEH_LoadFile(filename);
    }

    I_EndGlob(glob);""", """    (void) path;   // NucleoOS: no autoload directories""")
edit("d_main.c", "    D_AddFile(iwadfile);\n", "    D_AddFile(iwadfile);\n    nv_iwad_numlumps = numlumps;\n")
edit("d_main.c", "void DG_CheckDemoStatusAtExit(void)", "static int nv_iwad_numlumps;   // lumps of the IWAD: the PWADs' come after\n\nvoid DG_CheckDemoStatusAtExit(void)")
edit("r_data.c", '#include "p_local.h"\n', '#include "p_local.h"\n\nvoid R_InitTextures(void);\n')
edit("d_main.c", "    modifiedgame = W_ParseCommandLine();\n", """    modifiedgame = W_ParseCommandLine();

#ifdef FEATURE_DEHACKED
    {   // NucleoOS: DEHACKED lumps of the PWADs, in load order
        int i;
        for (i = nv_iwad_numlumps; i < numlumps; ++i)
        {
            if (!strncmp(lumpinfo[i].name, "DEHACKED", 8))
            {
                DEH_LoadLump(i, true, true);
            }
        }
    }
#endif
""")
# Texture definitions merged across the IWAD and the PWADs (ports/doom/nv_textures.inc): the
# vanilla loader stays in the file under another name, unused.
shutil.copyfile(os.path.join(os.path.dirname(os.path.abspath(__file__)), "nv_textures.inc"),
                os.path.join(out, "nv_textures.inc"))
edit("r_data.c", "void R_InitTextures (void)\n{", "static void R_InitTextures_vanilla (void)\n{")
edit("r_data.c", '#include <stdio.h>\n', '#include <stdio.h>\n#include <ctype.h>\n#include <strings.h>\n')
with open(os.path.join(out, "r_data.c"), "ab") as f:
    f.write(b'\n#include "nv_textures.inc"\n')
# w_merge.c (Chocolate) against doomgeneric's lumpinfo array of structs: DoMerge hands its new
# pointer list over instead of swapping lumpinfo, and the tail of the file (W_MergeFile & co.)
# comes from ports/doom/nv_merge_tail.inc.
edit("w_merge.c", '#include "z_zone.h"\n',
     '#include "z_zone.h"\n\nstatic lumpinfo_t **nv_merged;   // DoMerge result, copied back by W_MergeFile\nstatic int nv_merged_num;\n')
edit("w_merge.c", "    free(lumpinfo);\n    lumpinfo = newlumps;\n    numlumps = num_newlumps;\n}",
     "    nv_merged = newlumps;\n    nv_merged_num = num_newlumps;\n}")
_p = os.path.join(out, "w_merge.c")
_b = open(_p, "rb").read().decode("latin-1").replace("\r\n", "\n")
_b = _b[:_b.index("void W_PrintDirectory(void)")]
_b += open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "nv_merge_tail.inc"), encoding="utf-8").read()
open(_p, "wb").write(_b.encode("latin-1"))
for _f in ("W_MergeFile(char *filename)", "W_NWTDashMerge(char *filename)"):
    edit("w_merge.h", _f, _f.replace("char *", "const char *"))
edit("w_merge.h", "W_NWTMergeFile(char *filename, int flags)", "W_NWTMergeFile(const char *filename, int flags)")
# Sprites: merged PWAD sprite sets break vanilla's strict checks (a frame with both a rot=0 lump and
# rotations, two lumps for one rotation, missing rotations, frames past Z). Resolve them like
# PrBoom (the last lump wins, gaps are filled) and never draw a frame that doesn't exist.
edit("r_things.c", """    if (frame >= 29 || rotation > 8)
\tI_Error("R_InstallSpriteLump: "
\t\t"Bad frame characters in lump %i", lump);""", """    if (frame >= 29 || rotation > 8)
\treturn;   // NucleoOS: not a sprite frame this engine can show""")
edit("r_things.c", """\tif (sprtemp[frame].rotate == false)
\t    I_Error ("R_InitSprites: Sprite %s frame %c has "
\t\t     "multip rot=0 lump", spritename, 'A'+frame);

\tif (sprtemp[frame].rotate == true)
\t    I_Error ("R_InitSprites: Sprite %s frame %c has rotations "
\t\t     "and a rot=0 lump", spritename, 'A'+frame);
""", "\t// NucleoOS: a later rot=0 lump replaces whatever this frame had\n")
edit("r_things.c", """    if (sprtemp[frame].rotate == false)
\tI_Error ("R_InitSprites: Sprite %s frame %c has rotations "
\t\t "and a rot=0 lump", spritename, 'A'+frame);
""", """    if (sprtemp[frame].rotate == false)
    {   // NucleoOS: rotations after a rot=0 lump replace it
\tfor (r = 0; r < 8; r++)
\t    sprtemp[frame].lump[r] = -1;
    }
""")
edit("r_things.c", """    if (sprtemp[frame].lump[rotation] != -1)
\tI_Error ("R_InitSprites: Sprite %s : %c : %c "
\t\t "has two lumps mapped to it",
\t\t spritename, 'A'+frame, '1'+rotation);
""", "    // NucleoOS: a later lump for the same rotation wins\n")
edit("r_things.c", """\t      case -1:
\t\t// no rotations were found for that frame at all
\t\tI_Error ("R_InitSprites: No patches found "
\t\t\t "for %s frame %c", spritename, frame+'A');
\t\tbreak;""", """\t      case -1:
\t\t// NucleoOS: no lump for this frame: reuse frame A's if there is one
\t\tif (frame > 0 && sprtemp[0].rotate != -1)
\t\t    sprtemp[frame] = sprtemp[0];
\t\telse
\t\t    maxframe = frame;   // cut the sprite here
\t\tbreak;""")
edit("r_things.c", """\t\tfor (rotation=0 ; rotation<8 ; rotation++)
\t\t    if (sprtemp[frame].lump[rotation] == -1)
\t\t\tI_Error ("R_InitSprites: Sprite %s frame %c "
\t\t\t\t "is missing rotations",
\t\t\t\t spritename, frame+'A');""", """\t\tfor (rotation=0 ; rotation<8 ; rotation++)
\t\t    if (sprtemp[frame].lump[rotation] == -1)
\t\t    {   // NucleoOS: fill a missing rotation with any rotation present
\t\t\tint k;
\t\t\tfor (k = 0; k < 8 && sprtemp[frame].lump[k] == -1; k++) ;
\t\t\tsprtemp[frame].lump[rotation] = k < 8 ? sprtemp[frame].lump[k] : 0;
\t\t\tsprtemp[frame].flip[rotation] = k < 8 ? sprtemp[frame].flip[k] : 0;
\t\t    }""")
edit("r_things.c", """    if ( (thing->frame&FF_FRAMEMASK) >= sprdef->numframes )
\tI_Error ("R_ProjectSprite: invalid sprite frame %i : %i ",
\t\t thing->sprite, thing->frame);""", """    if ( (thing->frame&FF_FRAMEMASK) >= sprdef->numframes )
\treturn;   // NucleoOS: a frame the sprite set lacks is simply not drawn""")
edit("r_things.c", """    if ( (psp->state->frame & FF_FRAMEMASK)  >= sprdef->numframes)
\tI_Error ("R_ProjectSprite: invalid sprite frame %i : %i ",
\t\t psp->state->sprite, psp->state->frame);""", """    if ( (psp->state->frame & FF_FRAMEMASK)  >= sprdef->numframes)
\treturn;   // NucleoOS: see R_ProjectSprite""")
# Animations whose first/last texture or flat a PWAD dropped: skip the animation (vanilla: I_Error)
edit("p_spec.c", """\tif (lastanim->numpics < 2)
\t    I_Error ("P_InitPicAnims: bad cycle from %s to %s",
\t\t     startname, endname);""", """\tif (lastanim->numpics < 2)
\t    continue;   // NucleoOS: a broken cycle is skipped, not fatal""")
# Adaptive zone: take the biggest zone (8 MB down to 4) that still leaves 2.5 MB of heap for the
# WAD directories, sound, music and DEH tables - the linear memory the board could give this run
# decides, not a fixed number.
edit("i_system.c", """        zonemem = malloc(*size);

        // Failed to allocate?  Reduce zone size until we reach a size
        // that is acceptable.
""", """        zonemem = malloc(*size);
        if (zonemem != NULL)
        {
            void *reserve = malloc(2560 * 1024);   // NucleoOS: room left for everything else?
            if (reserve == NULL)
            {
                free(zonemem);
                zonemem = NULL;
            }
            else
            {
                free(reserve);
            }
        }

        // Failed to allocate?  Reduce zone size until we reach a size
        // that is acceptable.
""")
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
