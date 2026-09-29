// Host stand-in for nv_hal's nv_usb_storage_slot_of(): "/usbN" or "/usbN/..." (N = 0..6) is a mounted
// drive, anything else is not. Mirrors the device's contract, not its mount table.
#include <string.h>

int nv_usb_storage_slot_of(const char *path) {
    if (!path || strncmp(path, "/usb", 4) != 0) return -1;
    const char d = path[4];
    if (d < '0' || d > '6') return -1;
    return (path[5] == '\0' || path[5] == '/') ? d - '0' : -1;
}
