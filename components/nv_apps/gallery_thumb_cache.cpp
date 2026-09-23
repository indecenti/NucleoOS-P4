// gallery_thumb_cache — see header.
#include "gallery_thumb_cache.h"
#include "gallery_jpeg_hw.h"

#include "nv_sd.h"
#include "esp_heap_caps.h"

#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

constexpr char   kCacheDir[]   = "/sdcard/.thumbs";
constexpr int    kThumbW       = GALLERY_THUMB_W;
constexpr int    kThumbH       = GALLERY_THUMB_H;
constexpr size_t kPixLen       = (size_t)kThumbW * kThumbH * 2;
constexpr size_t kPathCap      = 160;   // matches gallery_app.cpp's kMaxPathLen
constexpr size_t kNameFragCap  = 40;    // cosmetic basename portion of the cache filename
constexpr uint64_t kMinFreeForNewThumb = 256u * 1024u;  // don't attempt a brand-new file under this

uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

const char *basename_of(const char *posix_path) {
    const char *slash = strrchr(posix_path, '/');
    return slash ? slash + 1 : posix_path;
}

// Fixed-buffer path composition below intentionally truncates pathological (>~150-char) source
// paths or basenames — a truncated cache path just risks a (harmless, self-consistent) filename
// collision, never a crash.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

// Collision-safe key: FNV-1a hash of the full source path as an 8-hex prefix, plus the basename
// for human debuggability when browsing the SD card — the hash prefix is the uniqueness guarantee.
void build_cache_path(const char *source_posix_path, char *out, size_t out_cap) {
    uint32_t h = fnv1a(source_posix_path);
    char frag[kNameFragCap + 1];
    snprintf(frag, sizeof(frag), "%s", basename_of(source_posix_path));
    snprintf(out, out_cap, "%s/%08x_%s.bin", kCacheDir, (unsigned)h, frag);
}

bool thumb_is_fresh(const char *source_posix_path, const char *thumb_posix) {
    struct stat thumb_st, src_st;
    if (stat(thumb_posix, &thumb_st) != 0) return false;             // no cache yet
    if (stat(source_posix_path, &src_st) != 0) return false;         // source vanished
    if (src_st.st_mtime > thumb_st.st_mtime) return false;           // source replaced/edited since
    // Torn/aborted write, or a thumb from a build with another geometry: rebuild it in place.
    return thumb_st.st_size == (long)(sizeof(lv_image_header_t) + kPixLen);
}

void fill_header(lv_image_header_t *hdr) {
    memset(hdr, 0, sizeof *hdr);
    hdr->magic  = LV_IMAGE_HEADER_MAGIC;
    hdr->cf     = LV_COLOR_FORMAT_RGB565;
    hdr->w      = (uint32_t)kThumbW;
    hdr->h      = (uint32_t)kThumbH;
    hdr->stride = (uint32_t)(kThumbW * 2);   // must be explicit — the bin decoder won't infer it
}

// Write to a .tmp file, flush+close, remove any existing dest (FATFS rename won't overwrite),
// rename into place. Never truncates a good thumbnail mid-write.
bool write_bin_atomic(const char *final_posix_path, const uint8_t *rgb565) {
    char tmp[kPathCap + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", final_posix_path);
    lv_image_header_t hdr;
    fill_header(&hdr);
    FILE *f = nv_sd_fopen(tmp, "wb");
    if (!f) return false;
    const size_t wrote_hdr = fwrite(&hdr, 1, sizeof(hdr), f);
    const size_t wrote_px  = fwrite(rgb565, 1, kPixLen, f);
    fflush(f);
    nv_sd_fclose(f);
    if (wrote_hdr != sizeof(hdr) || wrote_px != kPixLen) { remove(tmp); return false; }
    remove(final_posix_path);
    if (rename(tmp, final_posix_path) != 0) { remove(tmp); return false; }
    return true;
}
#pragma GCC diagnostic pop

uint8_t *alloc_px(void) {
    return (uint8_t *)heap_caps_aligned_alloc(GALLERY_PPA_ALIGN, gallery_ppa_align_size(kPixLen),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool load_bin(const char *thumb_posix, uint8_t *px) {
    FILE *f = nv_sd_fopen(thumb_posix, "rb");
    if (!f) return false;
    lv_image_header_t hdr;
    const bool ok = fread(&hdr, 1, sizeof hdr, f) == sizeof hdr &&
                    hdr.magic == LV_IMAGE_HEADER_MAGIC && hdr.cf == LV_COLOR_FORMAT_RGB565 &&
                    hdr.w == (uint32_t)kThumbW && hdr.h == (uint32_t)kThumbH &&
                    fread(px, 1, kPixLen, f) == kPixLen;
    nv_sd_fclose(f);
    return ok;
}

}  // namespace

bool gallery_thumb_get(const char *source_posix_path, bool video,
                       lv_image_dsc_t *dsc, uint8_t **px_out, bool *did_build) {
    *px_out = nullptr;
    if (did_build) *did_build = false;
    if (!source_posix_path || !dsc) return false;

    char thumb_posix[kPathCap];
    build_cache_path(source_posix_path, thumb_posix, sizeof(thumb_posix));
    uint8_t *px = alloc_px();
    if (!px) return false;

    bool ok = thumb_is_fresh(source_posix_path, thumb_posix) && load_bin(thumb_posix, px);
    if (!ok) {
        if (did_build) *did_build = true;
        // Free-space guard only for a brand-new file; a stale thumb is rebuilt in place.
        struct stat existing_st;
        uint64_t free_bytes = 0;
        const bool low = stat(thumb_posix, &existing_st) != 0 &&
                         nv_sd_info(nullptr, &free_bytes) && free_bytes < kMinFreeForNewThumb;
        gallery_raster_t r;
        const bool decoded = video ? gallery_jpeg_hw_decode_avi_poster(source_posix_path, &r)
                                   : gallery_jpeg_hw_decode_file(source_posix_path, &r);
        ok = decoded && gallery_ppa_scale_cover(&r, px, kThumbW, kThumbH, gallery_ppa_align_size(kPixLen));
        gallery_jpeg_hw_free(&r);
        if (ok && !low) {
            mkdir(kCacheDir, 0777);   // ok if it already exists
            write_bin_atomic(thumb_posix, px);   // best-effort: the thumb is usable either way
        }
    }
    if (!ok) { heap_caps_free(px); return false; }

    memset(dsc, 0, sizeof *dsc);
    fill_header(&dsc->header);
    dsc->data_size = (uint32_t)kPixLen;
    dsc->data      = px;
    *px_out = px;
    return true;
}

void gallery_thumb_cache_evict(const char *source_posix_path) {
    if (!source_posix_path) return;
    char thumb_posix[kPathCap];
    build_cache_path(source_posix_path, thumb_posix, sizeof(thumb_posix));
    remove(thumb_posix);
}
