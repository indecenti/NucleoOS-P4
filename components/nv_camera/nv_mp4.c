// nv_mp4 — see header. A deliberately small MP4 writer: one AVC track, one chunk, moov at the end.
#include "nv_mp4.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

struct nv_mp4 {
    FILE    *f;
    int      w, h, fps;
    uint32_t nsamples;
    uint32_t *sizes;        // AVCC byte size per sample (PSRAM, grows)
    uint32_t  sizes_cap;
    uint32_t *keys;         // 1-based sample numbers that are IDR (for stss)
    uint32_t  keys_cap, nkeys;
    uint8_t   sps[128];     uint32_t sps_len;
    uint8_t   pps[128];     uint32_t pps_len;
    long      mdat_pos;     // file offset of the mdat box header
    uint64_t  mdat_bytes;   // sample data bytes written into mdat
};

// -------- big-endian writers ---------------------------------------------------------------------
static void w8(FILE *f, uint8_t v)  { fwrite(&v, 1, 1, f); }
static void w16(FILE *f, uint16_t v){ uint8_t b[2] = {(uint8_t)(v>>8),(uint8_t)v}; fwrite(b,1,2,f); }
static void w32(FILE *f, uint32_t v){ uint8_t b[4] = {(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v}; fwrite(b,1,4,f); }
static void wtag(FILE *f, const char *t){ fwrite(t, 1, 4, f); }

static long box_begin(FILE *f, const char *type){ long p = ftell(f); w32(f, 0); wtag(f, type); return p; }
static void box_end(FILE *f, long p){
    long e = ftell(f);
    fseek(f, p, SEEK_SET); w32(f, (uint32_t)(e - p));
    fseek(f, e, SEEK_SET);
}
static void fullbox(FILE *f, uint8_t ver, uint32_t flags){ w8(f, ver); w8(f,(uint8_t)(flags>>16)); w8(f,(uint8_t)(flags>>8)); w8(f,(uint8_t)flags); }

// -------- open -----------------------------------------------------------------------------------
nv_mp4_t *nv_mp4_open(const char *path, int width, int height, int fps){
    if (!path || width <= 0 || height <= 0 || fps <= 0) return NULL;
    nv_mp4_t *m = (nv_mp4_t *)heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) return NULL;
    m->f = fopen(path, "wb");
    if (!m->f) { heap_caps_free(m); return NULL; }
    m->w = width; m->h = height; m->fps = fps;

    // ftyp
    long p = box_begin(m->f, "ftyp");
    wtag(m->f, "isom"); w32(m->f, 0x200);
    wtag(m->f, "isom"); wtag(m->f, "iso2"); wtag(m->f, "avc1"); wtag(m->f, "mp41");
    box_end(m->f, p);

    // mdat (streamed; size patched at close)
    m->mdat_pos = box_begin(m->f, "mdat");
    return m;
}

// -------- sample append --------------------------------------------------------------------------
static bool grow_u32(uint32_t **arr, uint32_t *cap, uint32_t need){
    if (need <= *cap) return true;
    uint32_t ncap = *cap ? *cap * 2 : 4096;
    if (ncap < need) ncap = need;
    uint32_t *n = (uint32_t *)heap_caps_realloc(*arr, ncap * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!n) return false;
    *arr = n; *cap = ncap; return true;
}

bool nv_mp4_write(nv_mp4_t *m, const uint8_t *p, size_t len, bool keyframe){
    if (!m || !p || len < 4) return false;
    uint32_t sample_bytes = 0;

    // Walk Annex-B NAL units (start codes 00 00 01, optionally preceded by extra 00).
    size_t start = 0; bool have = false;
    for (size_t j = 0; j + 3 <= len; ) {
        if (p[j] == 0 && p[j+1] == 0 && p[j+2] == 1) {
            if (have) {
                size_t end = j;
                while (end > start && p[end-1] == 0) end--;   // drop the next SC's leading zero(s)
                if (end > start) {
                    const uint8_t t = p[start] & 0x1F;
                    if (t == 7) { if (end-start <= sizeof m->sps) { memcpy(m->sps, p+start, end-start); m->sps_len = (uint32_t)(end-start); } }
                    else if (t == 8) { if (end-start <= sizeof m->pps) { memcpy(m->pps, p+start, end-start); m->pps_len = (uint32_t)(end-start); } }
                    else { w32(m->f, (uint32_t)(end-start)); fwrite(p+start, 1, end-start, m->f); sample_bytes += 4 + (uint32_t)(end-start); }
                }
            }
            start = j + 3; have = true; j += 3;
        } else j++;
    }
    if (have) {  // last NAL
        size_t end = len;
        while (end > start && p[end-1] == 0) end--;
        if (end > start) {
            const uint8_t t = p[start] & 0x1F;
            if (t == 7) { if (end-start <= sizeof m->sps) { memcpy(m->sps, p+start, end-start); m->sps_len = (uint32_t)(end-start); } }
            else if (t == 8) { if (end-start <= sizeof m->pps) { memcpy(m->pps, p+start, end-start); m->pps_len = (uint32_t)(end-start); } }
            else { w32(m->f, (uint32_t)(end-start)); fwrite(p+start, 1, end-start, m->f); sample_bytes += 4 + (uint32_t)(end-start); }
        }
    }

    if (sample_bytes == 0) return true;   // frame carried only SPS/PPS — no coded slice
    if (!grow_u32(&m->sizes, &m->sizes_cap, m->nsamples + 1)) return false;
    m->sizes[m->nsamples] = sample_bytes;
    m->mdat_bytes += sample_bytes;
    m->nsamples++;
    if (keyframe) { if (grow_u32(&m->keys, &m->keys_cap, m->nkeys + 1)) m->keys[m->nkeys++] = m->nsamples; }
    return true;
}

// -------- moov + close ---------------------------------------------------------------------------
// A tiny fixed 3x3 unity matrix (16.16 fixed point) used by tkhd/mvhd.
static void wmatrix(FILE *f){
    w32(f, 0x10000); w32(f, 0); w32(f, 0);
    w32(f, 0); w32(f, 0x10000); w32(f, 0);
    w32(f, 0); w32(f, 0); w32(f, 0x40000000);
}

long nv_mp4_close(nv_mp4_t *m){
    if (!m) return 0;
    FILE *f = m->f;
    long total = 0;

    box_end(f, m->mdat_pos);   // patch mdat size

    if (m->nsamples == 0 || m->sps_len < 4 || m->pps_len == 0) {
        // Nothing usable — just close; file will be an empty/partial mdat.
        fflush(f); total = ftell(f); fclose(f);
        if (m->sizes) heap_caps_free(m->sizes);
        if (m->keys)  heap_caps_free(m->keys);
        heap_caps_free(m);
        return total;
    }

    const uint32_t ts        = 90000;             // media timescale
    const uint32_t delta     = ts / (uint32_t)m->fps;
    const uint32_t duration  = delta * m->nsamples;
    const uint32_t mdat_data = (uint32_t)(m->mdat_pos + 8);   // first sample offset (single chunk)

    long moov = box_begin(f, "moov");

    // mvhd
    { long b = box_begin(f, "mvhd"); fullbox(f,0,0);
      w32(f,0); w32(f,0); w32(f,1000); w32(f, duration*1000/ts);
      w32(f,0x10000); w16(f,0x100); w16(f,0); w32(f,0); w32(f,0);
      wmatrix(f); for(int i=0;i<6;i++) { w32(f,0); } w32(f,2); box_end(f,b); }

    // trak
    long trak = box_begin(f, "trak");
    { long b = box_begin(f, "tkhd"); fullbox(f,0,3 /*enabled|in-movie*/);
      w32(f,0); w32(f,0); w32(f,1); w32(f,0); w32(f, duration*1000/ts);
      w32(f,0); w32(f,0); w16(f,0); w16(f,0); w16(f,0); w16(f,0);
      wmatrix(f); w32(f, (uint32_t)m->w<<16); w32(f, (uint32_t)m->h<<16); box_end(f,b); }

    // mdia
    long mdia = box_begin(f, "mdia");
    { long b = box_begin(f, "mdhd"); fullbox(f,0,0);
      w32(f,0); w32(f,0); w32(f,ts); w32(f,duration); w16(f,0x55c4 /*und*/); w16(f,0); box_end(f,b); }
    { long b = box_begin(f, "hdlr"); fullbox(f,0,0);
      w32(f,0); wtag(f,"vide"); w32(f,0); w32(f,0); w32(f,0);
      const char *nm = "NucleoOS Video"; fwrite(nm,1,strlen(nm)+1,f); box_end(f,b); }

    long minf = box_begin(f, "minf");
    { long b = box_begin(f, "vmhd"); fullbox(f,0,1); w16(f,0); w16(f,0); w16(f,0); w16(f,0); box_end(f,b); }
    { long b = box_begin(f, "dinf"); long dr = box_begin(f,"dref"); fullbox(f,0,0); w32(f,1);
      long ue = box_begin(f,"url "); fullbox(f,0,1); box_end(f,ue); box_end(f,dr); box_end(f,b); }

    long stbl = box_begin(f, "stbl");
    // stsd -> avc1 -> avcC
    { long b = box_begin(f,"stsd"); fullbox(f,0,0); w32(f,1);
        long a = box_begin(f,"avc1");
        for(int i=0;i<6;i++) { w8(f,0); }                   // reserved
        w16(f,1);                                           // data_ref_index
        w16(f,0); w16(f,0); w32(f,0); w32(f,0); w32(f,0);  // pre-defined/reserved
        w16(f,(uint16_t)m->w); w16(f,(uint16_t)m->h);
        w32(f,0x00480000); w32(f,0x00480000);              // 72 dpi h/v res
        w32(f,0); w16(f,1);                                 // reserved + frame_count
        for(int i=0;i<32;i++) { w8(f,0); }                  // compressorname
        w16(f,0x18); w16(f,0xFFFF);                         // depth + pre-defined
          long c = box_begin(f,"avcC");
          w8(f,1); w8(f,m->sps[1]); w8(f,m->sps[2]); w8(f,m->sps[3]);
          w8(f,0xFF);                                       // 6 bits reserved + lengthSizeMinusOne=3
          w8(f,0xE1);                                       // 3 bits reserved + numOfSPS=1
          w16(f,(uint16_t)m->sps_len); fwrite(m->sps,1,m->sps_len,f);
          w8(f,1);                                          // numOfPPS
          w16(f,(uint16_t)m->pps_len); fwrite(m->pps,1,m->pps_len,f);
          box_end(f,c);
        box_end(f,a);
      box_end(f,b); }
    // stts (all samples same delta)
    { long b = box_begin(f,"stts"); fullbox(f,0,0); w32(f,1); w32(f,m->nsamples); w32(f,delta); box_end(f,b); }
    // stss (sync samples) — omit if every frame is a keyframe
    if (m->nkeys && m->nkeys < m->nsamples) {
        long b = box_begin(f,"stss"); fullbox(f,0,0); w32(f,m->nkeys);
        for (uint32_t i=0;i<m->nkeys;i++) { w32(f,m->keys[i]); }
        box_end(f,b);
    }
    // stsc: 1 chunk holds all samples
    { long b = box_begin(f,"stsc"); fullbox(f,0,0); w32(f,1); w32(f,1); w32(f,m->nsamples); w32(f,1); box_end(f,b); }
    // stsz: per-sample sizes
    { long b = box_begin(f,"stsz"); fullbox(f,0,0); w32(f,0); w32(f,m->nsamples);
      for (uint32_t i=0;i<m->nsamples;i++) { w32(f,m->sizes[i]); }
      box_end(f,b); }
    // stco: single chunk offset
    { long b = box_begin(f,"stco"); fullbox(f,0,0); w32(f,1); w32(f,mdat_data); box_end(f,b); }

    box_end(f, stbl);
    box_end(f, minf);
    box_end(f, mdia);
    box_end(f, trak);
    box_end(f, moov);

    fflush(f);
    total = ftell(f);
    fclose(f);
    if (m->sizes) heap_caps_free(m->sizes);
    if (m->keys)  heap_caps_free(m->keys);
    heap_caps_free(m);
    return total;
}
