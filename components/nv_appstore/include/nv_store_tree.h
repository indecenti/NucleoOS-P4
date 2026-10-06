// nv_store_tree — the folder trees of system content packs (nv_store_pkg.h, "nucleoos-data-v2" kinds
// i/t/u). Pure code, no ESP-IDF: host unit tests + fuzzer in tests/host (target "content").
//
// Three pieces:
//   * the tree index ("i" file): every file of a tree with its sha256 and size, signed through the
//     pack.sig line that lists the index's own sha256;
//   * a streaming ustar reader for the tree archive ("t"/"u" file), fed straight from the SD card;
//   * the decision table that finishes or undoes a folder swap a power cut interrupted.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nv_store_tree {

constexpr int      kPathMax  = 160;                  // incl. NUL, relative to the tree root
constexpr int      kDepthMax = 8;                    // folder levels
constexpr int      kIndexMax = 4096;                 // files per tree
constexpr uint64_t kFileMax  = 0xFFFFFFFFull;        // FAT32

// A relative path inside a tree: 1..159 chars of [A-Za-z0-9._-+] segments joined by '/', no empty, "."
// or ".." segment, no leading or trailing '/', at most kDepthMax segments.
bool path_ok(const char *path, size_t len);

// ---- index ----------------------------------------------------------------------------------------
//
//     nucleoos-tree-v1
//     <sha256 lowercase hex> <size> <path>     one line per file, paths strictly ascending (bytewise)
//
// Folders are implied by the paths. At least one file. The text is kept by the caller (the index
// points into it).
struct Index {
    const char *text;
    size_t      len;
    int         n;
    uint32_t    off[kIndexMax];                      // start of each file line in text
};

struct Entry {
    char     path[kPathMax];
    uint64_t size;
    uint8_t  sha256[32];
};

bool parse_index(const char *text, size_t len, Index *out);
// File i (0 <= i < n) of a parsed index.
bool index_entry(const Index &ix, int i, Entry *out);
// The position of `path` (binary search), or -1.
int index_find(const Index &ix, const char *path);
// Total bytes of every file.
uint64_t index_total(const Index &ix);

// ---- ustar reader -----------------------------------------------------------------------------------
//
// Accepts what Python's tarfile writes with format=USTAR_FORMAT (and GNU's "ustar  " magic): regular
// files and folders only, octal sizes, checksummed headers, path = prefix "/" name with any leading
// "./" dropped. Anything else (links, devices, pax/GNU long-name headers, a bad checksum, an unsafe
// path, data after the end marker) is an error: the archive is signed, so a surprise means a bug.
class Sink {
public:
    virtual ~Sink() {}
    virtual bool dir(const char *path) = 0;                      // a folder (may come before or never)
    virtual bool begin(const char *path, uint64_t size) = 0;     // a file starts
    virtual bool data(const uint8_t *p, size_t n) = 0;           // its bytes, in order
    virtual bool end() = 0;                                      // all `size` bytes were given
};

class TarReader {
public:
    TarReader() { reset(); }
    void reset();
    // Feed the next n bytes of the archive. False on a malformed archive or when the sink refuses.
    bool feed(const uint8_t *p, size_t n, Sink &sink);
    // The end-of-archive marker was seen (two zero blocks, or one followed by end of input) and no
    // file is half-read. Call after the last feed().
    bool finished() const;

private:
    bool header(Sink &sink);
    uint8_t  hdr_[512];
    size_t   hdr_n_;
    uint64_t left_;                                              // file bytes still to come
    uint32_t pad_;                                               // padding bytes to skip after them
    int      zeros_;                                             // zero blocks seen in a row
    bool     in_file_, bad_;
};

// ---- interrupted folder swap ------------------------------------------------------------------------
//
// Replacing folder D: the new tree is extracted and verified in D.new; the journal names D; D -> D.old;
// D.new -> D; the journal goes; D.old is deleted. After a reboot, what exists decides what to finish.
enum SwapAction : unsigned {     // values = execution order
    SWAP_RM_STALE   = 1,          // delete a leftover D.old before D can move there
    SWAP_DIR_TO_OLD = 2,          // rename D -> D.old
    SWAP_NEW_TO_DIR = 4,          // rename D.new -> D
    SWAP_OLD_TO_DIR = 8,          // rename D.old -> D (roll back: the new tree is gone)
    SWAP_RM_NEW     = 16,         // delete D.new (an extraction that never finished)
    SWAP_RM_OLD     = 32,         // delete D.old
};
// The actions to run, lowest bit first; afterwards the journal is deleted. `journal`: the journal names
// D (the new tree was complete and verified when it was written). D always exists afterwards when any
// version of it existed before.
unsigned swap_recover(bool journal, bool dir, bool dir_new, bool dir_old);

}  // namespace nv_store_tree
