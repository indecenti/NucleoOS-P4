// gallery_thumb_cache — see header.
#include "gallery_thumb_cache.h"
#include "gallery_jpeg_hw.h"

#include "nv_sd.h"
#include "lvgl.h"
#include "esp_heap_caps.h"

#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

constexpr char   kCacheDir[]   = "/sdcard/.thumbs";
constexpr int    kThumbW       = 160;
constexpr int    kThumbH       = 150;
constexpr size_t kPathCap      = 160;   // matches gallery_app.cpp's kMaxPathLen
constexpr size_t kNameFragCap  = 40;    // cosmetic basename portion of the cache filename
constexpr uint64_t kMinFreeForNewThumb = 64u * 1024u;  // don't attempt a brand-new file under this

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
// collision, never a crash. Same rationale/pattern as gallery_app.cpp's own add_item()/scan_dir().
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

// Collision-safe key: FNV-1a hash of the full source path (unique across the 3 scanned dirs even
// when basenames collide) as an 8-hex prefix, plus the basename itself for human debuggability
// when browsing the SD card — the hash prefix is the actual uniqueness guarantee.
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
    if (thumb_st.st_size < (long)sizeof(lv_image_header_t)) return false;  // torn/aborted write
    return true;
}

// Byte-for-byte structural match of nv_backup.cpp's nv_backup_export(): write to a .tmp file,
// flush+close, remove any existing dest (FATFS rename won't overwrite), rename into place. Never
// truncates a good thumbnail mid-write.
bool write_bin_atomic(const char *final_posix_path, int w, int h,
                       const uint8_t *rgb565, size_t pixel_len) {
    char tmp[kPathCap + 8];   // room for the ".tmp" suffix (silences -Werror=format-truncation)
    snprintf(tmp, sizeof(tmp), "%s.tmp", final_posix_path);

    lv_image_header_t hdr = {};
    hdr.magic      = LV_IMAGE_HEADER_MAGIC;
    hdr.cf         = LV_COLOR_FORMAT_RGB565;
    hdr.flags      = 0;
    hdr.w          = (uint32_t)w;
    hdr.h          = (uint32_t)h;
    hdr.stride     = (uint32_t)(w * 2);   // must be explicit — the bin decoder won't infer it
    hdr.reserved_2 = 0;

    FILE *f = nv_sd_fopen(tmp, "wb");
    if (!f) return false;
    size_t wrote_hdr = fwrite(&hdr, 1, sizeof(hdr), f);
    size_t wrote_px  = fwrite(rgb565, 1, pixel_len, f);
    fflush(f);
    nv_sd_fclose(f);
    if (wrote_hdr != sizeof(hdr) || wrote_px != pixel_len) { remove(tmp); return false; }

    remove(final_posix_path);              // FATFS rename won't overwrite an existing dest
    if (rename(tmp, final_posix_path) != 0) { remove(tmp); return false; }
    return true;
}
#pragma GCC diagnostic pop

}  // namespace

bool gallery_thumb_cache_get_or_build(const char *source_posix_path, int src_w, int src_h,
                                       char *out_thumb_path, size_t out_cap,
                                       bool *out_did_build) {
    out_thumb_path[0] = '\0';
    if (out_did_build) *out_did_build = false;
    if (!source_posix_path || src_w <= 0 || src_h <= 0) return false;

    char thumb_posix[kPathCap];
    build_cache_path(source_posix_path, thumb_posix, sizeof(thumb_posix));

    if (thumb_is_fresh(source_posix_path, thumb_posix)) {
        snprintf(out_thumb_path, out_cap, "S:%s", thumb_posix);
        return true;
    }
    if (out_did_build) *out_did_build = true;

    // Free-space guard: only gate a BRAND-NEW cache file, not a rebuild over an existing stale
    // one — the file is tiny (~47KB) and a stale-forever thumbnail is worse than the small risk
    // of a low-space write attempt.
    struct stat existing_st;
    const bool already_exists = (stat(thumb_posix, &existing_st) == 0);
    if (!already_exists) {
        uint64_t free_bytes = 0;
        if (nv_sd_info(nullptr, &free_bytes) && free_bytes < kMinFreeForNewThumb) return false;
    }

    mkdir(kCacheDir, 0777);   // ok if it already exists

    uint8_t *decoded = nullptr;
    size_t   decoded_len = 0;
    if (!gallery_jpeg_hw_decode_file(source_posix_path, src_w, src_h, &decoded, &decoded_len))
        return false;

    const size_t thumb_cap = gallery_ppa_align_size((size_t)kThumbW * kThumbH * 2);
    uint8_t *thumb = (uint8_t *)heap_caps_aligned_alloc(GALLERY_PPA_ALIGN, thumb_cap,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!thumb) { gallery_jpeg_hw_free(decoded); return false; }

    bool ok = gallery_ppa_scale_stretch(decoded, src_w, src_h, thumb, kThumbW, kThumbH, thumb_cap);
    gallery_jpeg_hw_free(decoded);
    if (ok) {
        ok = write_bin_atomic(thumb_posix, kThumbW, kThumbH, thumb, (size_t)kThumbW * kThumbH * 2);
    }
    heap_caps_free(thumb);
    if (!ok) return false;

    snprintf(out_thumb_path, out_cap, "S:%s", thumb_posix);
    return true;
}

bool gallery_thumb_cache_lookup(const char *source_posix_path,
                                 char *out_thumb_path, size_t out_cap) {
    out_thumb_path[0] = '\0';
    if (!source_posix_path) return false;
    char thumb_posix[kPathCap];
    build_cache_path(source_posix_path, thumb_posix, sizeof(thumb_posix));
    if (!thumb_is_fresh(source_posix_path, thumb_posix)) return false;
    snprintf(out_thumb_path, out_cap, "S:%s", thumb_posix);
    return true;
}

void gallery_thumb_cache_evict(const char *source_posix_path) {
    if (!source_posix_path) return;
    char thumb_posix[kPathCap];
    build_cache_path(source_posix_path, thumb_posix, sizeof(thumb_posix));
    remove(thumb_posix);
}
