// files_ops — Files' background copy / move / delete-tree worker (SD card <-> USB drives).
// One job at a time on its own task, so a multi-GB copy neither blocks the LVGL thread nor the
// shared nv_bgwork queue, and keeps running after Files is closed (a notification reports the end).
// The job holds a removal-safe session on every volume it touches: ejecting a drive mid-copy is
// refused instead of tearing the copy.
#pragma once
#include <stdint.h>

enum FopKind : uint8_t { FOP_COPY, FOP_MOVE, FOP_DELETE };

enum FopResult : uint8_t { FOP_OK, FOP_BUSY, FOP_BAD_ARGS, FOP_INTO_SELF };

// Copy/move `src` (file or folder) INTO folder `dst_dir` (a name clash gets " (2)"...). DELETE
// removes `src` recursively (dst_dir ignored). Returns immediately; FOP_OK = job queued.
FopResult fop_start(FopKind kind, const char *src, const char *dst_dir);

void fop_cancel(void);

struct FopStatus {
    bool     busy;
    FopKind  kind;
    unsigned pct;          // 0..100 by bytes
    char     current[64];  // file being processed
};
void fop_status(FopStatus *out);

// Bumped when a job finishes (the list on screen may have changed).
uint32_t fop_generation(void);
