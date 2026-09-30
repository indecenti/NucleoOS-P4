// nv_textures.inc — included at the end of r_data.c by ports/doom/patch_sources.py.
//
// R_InitTextures that MERGES texture definitions instead of taking only the last TEXTURE1/2:
// the PWAD's textures come first (its PNAMES), then every IWAD texture whose name the PWAD does
// not define (the IWAD's own PNAMES). An episode replacement for Ultimate Doom (SIGIL, DTWID,
// Double Impact...) ships a TEXTURE1 without Freedoom's extra textures; with vanilla loading the
// episodes it does not replace came out with placeholder walls. A patch that can't be found is
// dropped from its texture instead of stopping the game.

typedef struct { int tex, pnames; } nv_texsrc_t;

static int NV_FirstLump(const char *name) {
    for (int i = 0; i < (int)numlumps; i++)
        if (!strncasecmp(lumpinfo[i].name, name, 8)) return i;
    return -1;
}

static int *NV_PatchLookup(int pnames_lump, int *count) {
    char name[9];
    char *names = W_CacheLumpNum(pnames_lump, PU_STATIC);
    const int n = LONG(*((int *)names));
    int *lookup = Z_Malloc((n > 0 ? n : 1) * sizeof(*lookup), PU_STATIC, NULL);
    name[8] = 0;
    for (int i = 0; i < n; i++) {
        M_StringCopy(name, names + 4 + i * 8, sizeof(name));
        lookup[i] = W_CheckNumForName(name);
    }
    W_ReleaseLumpNum(pnames_lump);
    *count = n;
    return lookup;
}

void R_InitTextures(void) {
    nv_texsrc_t src[4];
    int nsrc = 0, capacity = 0;
    const int pn = W_GetNumForName(DEH_String("PNAMES"));
    const int t1 = W_GetNumForName(DEH_String("TEXTURE1"));
    const int t2 = W_CheckNumForName(DEH_String("TEXTURE2"));
    const int ipn = NV_FirstLump("PNAMES");
    const int it1 = NV_FirstLump("TEXTURE1");
    const int it2 = NV_FirstLump("TEXTURE2");

    src[nsrc++] = (nv_texsrc_t){t1, pn};
    if (t2 >= 0) src[nsrc++] = (nv_texsrc_t){t2, pn};
    if (it1 >= 0 && it1 != t1) src[nsrc++] = (nv_texsrc_t){it1, ipn};
    if (it2 >= 0 && it2 != t2) src[nsrc++] = (nv_texsrc_t){it2, ipn};

    for (int s = 0; s < nsrc; s++) {
        const int *m = W_CacheLumpNum(src[s].tex, PU_STATIC);
        capacity += LONG(*m);
        W_ReleaseLumpNum(src[s].tex);
    }
    textures = Z_Malloc(capacity * sizeof(*textures), PU_STATIC, 0);
    texturecolumnlump = Z_Malloc(capacity * sizeof(*texturecolumnlump), PU_STATIC, 0);
    texturecolumnofs = Z_Malloc(capacity * sizeof(*texturecolumnofs), PU_STATIC, 0);
    texturecomposite = Z_Malloc(capacity * sizeof(*texturecomposite), PU_STATIC, 0);
    texturecompositesize = Z_Malloc(capacity * sizeof(*texturecompositesize), PU_STATIC, 0);
    texturewidthmask = Z_Malloc(capacity * sizeof(*texturewidthmask), PU_STATIC, 0);
    textureheight = Z_Malloc(capacity * sizeof(*textureheight), PU_STATIC, 0);

    // names already defined: small open-addressing hash of texture indices
    const int hsize = capacity * 2 + 1;
    int *hash = Z_Malloc(hsize * sizeof(int), PU_STATIC, 0);
    for (int i = 0; i < hsize; i++) hash[i] = -1;

    numtextures = 0;
    int cur_pn = -1, npatches = 0, merged = 0;
    int *patchlookup = NULL;
    for (int s = 0; s < nsrc; s++) {
        if (src[s].pnames != cur_pn) {
            if (patchlookup) Z_Free(patchlookup);
            patchlookup = NV_PatchLookup(src[s].pnames, &npatches);
            cur_pn = src[s].pnames;
        }
        int *maptex = W_CacheLumpNum(src[s].tex, PU_STATIC);
        const int count = LONG(*maptex), maxoff = W_LumpLength(src[s].tex);
        int *directory = maptex + 1;
        for (int k = 0; k < count; k++, directory++) {
            const int offset = LONG(*directory);
            if (offset < 0 || offset > maxoff) break;   // damaged directory: keep what we have
            maptexture_t *mtexture = (maptexture_t *)((byte *)maptex + offset);

            // skip a name that is already defined (the PWAD wins)
            unsigned h = 5381;
            for (int c = 0; c < 8 && mtexture->name[c]; c++) h = h * 33 + (unsigned)toupper((unsigned char)mtexture->name[c]);
            int slot = (int)(h % (unsigned)hsize), dup = 0;
            while (hash[slot] >= 0) {
                if (!strncasecmp(textures[hash[slot]]->name, mtexture->name, 8)) { dup = 1; break; }
                slot = (slot + 1) % hsize;
            }
            if (dup) continue;

            const int pc = SHORT(mtexture->patchcount);
            texture_t *texture = Z_Malloc(sizeof(texture_t) + sizeof(texpatch_t) * (pc > 0 ? pc - 1 : 0), PU_STATIC, 0);
            texture->width = SHORT(mtexture->width);
            texture->height = SHORT(mtexture->height);
            memcpy(texture->name, mtexture->name, sizeof(texture->name));
            mappatch_t *mpatch = &mtexture->patches[0];
            texpatch_t *patch = &texture->patches[0];
            int kept = 0;
            for (int j = 0; j < pc; j++, mpatch++) {
                const int pi = SHORT(mpatch->patch);
                const int lump = (pi >= 0 && pi < npatches) ? patchlookup[pi] : -1;
                if (lump < 0) continue;   // missing patch: leave it out
                patch->originx = SHORT(mpatch->originx);
                patch->originy = SHORT(mpatch->originy);
                patch->patch = lump;
                patch++;
                kept++;
            }
            if (kept == 0) { Z_Free(texture); continue; }
            texture->patchcount = kept;

            const int i = numtextures++;
            textures[i] = texture;
            hash[slot] = i;
            if (s >= (t2 >= 0 ? 2 : 1)) merged++;
            texturecolumnlump[i] = Z_Malloc(texture->width * sizeof(**texturecolumnlump), PU_STATIC, 0);
            texturecolumnofs[i] = Z_Malloc(texture->width * sizeof(**texturecolumnofs), PU_STATIC, 0);
            int w = 1;
            while (w * 2 <= texture->width) w <<= 1;
            texturewidthmask[i] = w - 1;
            textureheight[i] = texture->height << FRACBITS;
        }
        W_ReleaseLumpNum(src[s].tex);
    }
    if (patchlookup) Z_Free(patchlookup);
    Z_Free(hash);
    if (merged) printf("R_InitTextures: %d textures kept from the IWAD\n", merged);

    for (int i = 0; i < numtextures; i++) R_GenerateLookup(i);
    texturetranslation = Z_Malloc((numtextures + 1) * sizeof(*texturetranslation), PU_STATIC, 0);
    for (int i = 0; i < numtextures; i++) texturetranslation[i] = i;
    GenerateTextureHashTable();
}
