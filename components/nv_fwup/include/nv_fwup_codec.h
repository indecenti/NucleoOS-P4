// nv_fwup_codec — streaming raw-deflate pack/unpack of a firmware image through callbacks, so the
// safety copy of the system fits the `assets` partition (images compress to ~58 %). Pure code over
// the miniz API: the ESP32-P4 ROM's tdefl/tinfl on the device, vendored miniz in tests/host.
// The caller owns every buffer (PSRAM on the device); nothing here allocates.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nv_fwup_codec {

// Read n bytes at offset off of the source / append n bytes to the sink. False aborts the job.
typedef bool (*ReadFn)(void *user, uint32_t off, void *buf, size_t n);
typedef bool (*WriteFn)(void *user, const void *buf, size_t n);

enum class Err : uint8_t { Ok, Param, Read, Write, Corrupt, Size };
const char *err_str(Err e);

// Work areas: deflate needs a tdefl_compressor plus an output buffer; inflate needs a
// tinfl_decompressor plus the 32 KB circular dictionary. Sizes in bytes.
size_t deflate_work_size(void);
size_t inflate_work_size(void);
constexpr size_t kInflateDict = 32768;   // TINFL_LZ_DICT_SIZE

// Pack raw_size bytes from `rd` into `wr`. `in_buf`/`out_buf` are staging buffers (any size >= 1 KB;
// 32 KB is a good flash/SD transfer). `probes` 1..128 trades speed for size (6 ~ zlib level 2).
// *stored_out = bytes written.
Err deflate(ReadFn rd, void *ru, uint32_t raw_size, WriteFn wr, void *wu, void *work,
            uint8_t *in_buf, size_t in_cap, uint8_t *out_buf, size_t out_cap, int probes,
            uint32_t *stored_out);

// Unpack stored_size bytes of raw deflate from `rd` into `wr`; the stream must decode to exactly
// raw_size bytes (more, fewer or a broken stream -> Size / Corrupt). `work` = inflate_work_size().
Err inflate(ReadFn rd, void *ru, uint32_t stored_size, WriteFn wr, void *wu, uint32_t raw_size,
            void *work, uint8_t *in_buf, size_t in_cap);

}  // namespace nv_fwup_codec
