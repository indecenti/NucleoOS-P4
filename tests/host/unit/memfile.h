// In-memory FILE* backed by a real file descriptor (memfd): vp_avi reads through fileno()/lseek()/
// read(), which fmemopen() streams don't have.
#pragma once
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

struct MemFile {
    int fd = -1;
    FILE *f = nullptr;
    MemFile() {
        fd = memfd_create("nv_host_test", 0);
        if (fd >= 0) f = fdopen(fd, "rb");
    }
    ~MemFile() { if (f) fclose(f); }
    // Replace the whole content (reused across fuzz iterations).
    bool set(const void *data, size_t n) {
        if (!f || ftruncate(fd, 0) != 0) return false;
        const uint8_t *p = (const uint8_t *)data;
        size_t done = 0;
        while (done < n) {
            const ssize_t w = pwrite(fd, p + done, n - done, (off_t)done);
            if (w <= 0) return false;
            done += (size_t)w;
        }
        return true;
    }
};
