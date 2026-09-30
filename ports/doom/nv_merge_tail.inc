// nv_merge_tail.inc - replaces the end of Chocolate's w_merge.c (from W_PrintDirectory on) for
// doomgeneric, whose lumpinfo is an array of lumpinfo_t (Chocolate: an array of pointers).
// The merge works on a temporary pointer view of the array, and DoMerge's pointer list is copied
// back into a fresh struct array. NWT-style merges are not used by the device front-end: they
// just add the file.

void W_PrintDirectory(void)
{
    unsigned int i, n;

    for (i = 0; i < numlumps; ++i)
    {
        for (n = 0; n < 8 && lumpinfo[i].name[n] != '\0'; ++n)
            putchar(lumpinfo[i].name[n]);
        putchar('\n');
    }
}

void W_MergeFile(const char *filename)
{
    int old_numlumps = numlumps;
    lumpinfo_t **view;
    unsigned int i;

    if (W_AddFile((char *) filename) == NULL)
        return;

    // pointer view of the lump array: IWAD first, the PWAD appended at the end
    view = malloc(numlumps * sizeof(*view));
    if (view == NULL)
        I_Error("W_MergeFile: out of memory");
    for (i = 0; i < numlumps; ++i)
        view[i] = &lumpinfo[i];

    iwad.lumps = view;
    iwad.numlumps = old_numlumps;
    pwad.lumps = view + old_numlumps;
    pwad.numlumps = numlumps - old_numlumps;

    SetupLists();
    GenerateSpriteList();
    DoMerge();   // leaves the merged list in nv_merged / nv_merged_num (see the patched DoMerge)

    {
        lumpinfo_t *arr = malloc(nv_merged_num * sizeof(*arr));
        if (arr == NULL)
            I_Error("W_MergeFile: out of memory");
        for (i = 0; i < (unsigned int) nv_merged_num; ++i)
            arr[i] = *nv_merged[i];
        free(nv_merged);
        free(view);
        free(lumpinfo);
        lumpinfo = arr;
        numlumps = nv_merged_num;
    }
}

void W_NWTMergeFile(const char *filename, int flags)
{
    (void) flags;
    W_AddFile((char *) filename);
}

void W_NWTDashMerge(const char *filename)
{
    W_AddFile((char *) filename);
}
