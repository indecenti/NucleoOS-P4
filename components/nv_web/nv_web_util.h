// nv_web_util — the pure string/path helpers behind the web API, split out of nv_web.cpp so they
// build on the PC with no ESP-IDF (tests/host unit-tests and fuzzes them). Everything here parses
// LAN input: query strings, JSON bodies and file paths from unauthenticated requests.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// Static docroot (the recovered shell + apps live here) and the web-OS logical FS root. The shell
// speaks LOGICAL paths ("/system/config/...", "/data/...", "/DCIM", ...); every /api/fs/* call is
// mapped under FS_ROOT. FS_ROOT is the WHOLE card ("/sdcard") so the web OS's file manager, photo
// viewer and media players see the real device content (DCIM, Recordings, music, notes), while its
// own config still lands tidily under /sdcard/system + /sdcard/data. `..` is rejected, and the
// served OS tree (/sdcard/web) is write-protected so the file manager can't delete itself.
// Macros (not constexpr vars) so string-literal concatenation like WEB_ROOT "/apps.json" works.
#define WEB_ROOT "/sdcard/web"
#define FS_ROOT  "/sdcard"

namespace nv_web_util {

// Percent-decode `in` into `out` (also '+' -> space), always NUL-terminated within n.
void url_decode(const char *in, char *out, size_t n);

// Content-Type for a file name, by extension (case-insensitive).
const char *mime_for(const char *path);

// Map a shell LOGICAL path to a physical one: "/x" -> FS_ROOT "/x", "/mnt/usbN/x" -> "/usbN/x".
// false on a path the LAN must not reach ("..", '\', "//", the NVS mirror).
bool map_fs(const char *logical, char *out, size_t n);

// false for the served web-OS tree (WEB_ROOT and below), which the file API must not modify.
bool fs_writable(const char *phys);

// A physical path the LAN may hand to nv_open: on the SD card, no "..", not the NVS mirror.
bool open_path_ok(const char *p);

// Minimal extractors for small JSON bodies: "key": <int> / "key":"<value>" (no unescaping).
long json_int(const char *body, const char *key, long dflt);
bool json_str(const char *body, const char *key, char *out, size_t n);

// Escape a string for embedding inside JSON double quotes (handles " \ and control chars).
void json_escape(char *out, size_t n, const char *src);

}  // namespace nv_web_util

// Provided by nv_hal (nv_usb_storage.h) on the device and by a stub in the host tests.
extern "C" int nv_usb_storage_slot_of(const char *path);
