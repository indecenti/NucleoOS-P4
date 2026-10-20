// inflate.c — deflate streams (ZIP members, the embedded fonts) with miniz's tinfl (MIT,
// ports/books/_src/miniz-3.1.2): the whole output in one buffer whose size is known in advance.
#define MINIZ_NO_TIME
#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#include "miniz_tinfl.c"

// Inflates src into out; zlib: the stream has a zlib header. Returns the bytes written, -1 on a
// damaged stream or one longer than out_n.
int bk_inflate(const void *src, size_t n, void *out, size_t out_n, int zlib) {
    const size_t r = tinfl_decompress_mem_to_mem(out, out_n, src, n,
                                                 (zlib ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0) |
                                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (r == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || r > 0x7FFFFFFF) return -1;
    return (int)r;
}

// The first bytes of a raw deflate stream (a picture's header): src may be the stream's start only.
// Returns how many bytes were produced (up to out_n), -1 on a damaged stream.
int bk_inflate_head(const void *src, size_t n, void *out, size_t out_n) {
    tinfl_decompressor d;
    tinfl_init(&d);
    size_t in = n, outl = out_n;
    const tinfl_status s = tinfl_decompress(&d, (const mz_uint8 *)src, &in, (mz_uint8 *)out, (mz_uint8 *)out, &outl,
                                            TINFL_FLAG_HAS_MORE_INPUT | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (s < 0) return -1;
    return (int)outl;
}
