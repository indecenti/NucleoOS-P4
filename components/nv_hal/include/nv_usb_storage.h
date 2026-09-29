// nv_usb_storage — USB mass storage HOST on the OTG-HS Type-C: pendrives, SSD enclosures and card
// readers (multi-slot readers expose one LUN per slot), directly or behind a USB hub.
//
// Each LUN gets a stable "slot" mounted at /usb0 … /usb6 (FAT12/16/32 + exFAT, MBR or GPT). Card
// readers are polled (TEST UNIT READY) so swapping the card remounts without re-plugging the hub.
// Bulk-Only Transport + SCSI are implemented here directly (the Espressif MSC component only
// handles LUN 0 and has no media-change handling).
//
// Threading: every call is safe from any task, but eject/format/refresh BLOCK for up to seconds
// (USB I/O) — never call them on the LVGL thread, use a worker (nv_bgwork). The list/get calls
// only copy cached state and never touch the bus.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_USB_STOR_SLOTS 7

typedef enum {
    NV_USB_STOR_EMPTY = 0,      // reader slot without a card / medium not ready yet
    NV_USB_STOR_MOUNTED,        // readable (and writable unless read_only)
    NV_USB_STOR_UNFORMATTED,    // medium present but no FAT/exFAT volume -> format offered
    NV_USB_STOR_EJECTED,        // user ejected: safe to remove; reinserting the card remounts
    NV_USB_STOR_ERROR,          // medium present but unreadable (I/O errors)
} nv_usb_stor_state_t;

typedef struct {
    bool     used;              // slot bound to a LUN of a connected device
    nv_usb_stor_state_t state;
    char     path[8];           // VFS mount point, "/usb0"
    char     label[24];         // volume label, or "" when the volume has none
    char     fs[8];             // "FAT12" "FAT16" "FAT32" "exFAT" (mounted only)
    char     vendor[9];         // SCSI INQUIRY vendor (trimmed)
    char     product[17];       // SCSI INQUIRY product (trimmed)
    uint64_t total_bytes;       // volume size (mounted) or medium size (unformatted)
    uint64_t free_bytes;        // UINT64_MAX while the first free-space scan is running
    bool     read_only;         // write-protect switch / device reports WP
    bool     removable;         // RMB bit: card reader slot or removable-media stick
    uint16_t vid, pid;
    uint8_t  addr, lun;         // USB address + logical unit
    uint8_t  speed;             // 0 low, 1 full, 2 high
} nv_usb_stor_info_t;

// NV_EV_USB_STORAGE payload, published from the storage worker task (subscribers only set flags;
// hop to the LVGL thread for UI).
typedef struct {
    int slot;
    nv_usb_stor_state_t state;      // new state
    nv_usb_stor_state_t prev;       // previous state (EMPTY for a freshly attached LUN)
    bool detached;                  // the whole device went away (slot released)
} nv_usb_stor_ev_t;

// Register the host client + worker. Needs usb_host_install (nv_usb_audio_init) — waits for it.
bool nv_usb_storage_init(void);

// Bumped on every visible change (mount, unmount, card swap, free-space update). UI polls it.
uint32_t nv_usb_storage_generation(void);

// Copies every slot in use (index order). Returns the count.
int  nv_usb_storage_list(nv_usb_stor_info_t *out, int max);
bool nv_usb_storage_get(int slot, nv_usb_stor_info_t *out);
int  nv_usb_storage_mounted_count(void);

// Slot of a path under a USB mount ("/usb2/DCIM/x.jpg" -> 2), else -1.
int  nv_usb_storage_slot_of(const char *path);

// Flush + unmount + SCSI eject. Fails (false) while files on the slot are still open through
// sessions after a short drain wait. Blocking.
bool nv_usb_storage_eject(int slot);

// Create a fresh volume (erases everything). exfat=false -> FAT32 (FAT16 for tiny media).
// label may be NULL. Blocking (seconds on big media).
bool nv_usb_storage_format(int slot, bool exfat, const char *label);

// Removal-safe sessions (same contract as nv_sd_session_*): begin fails when the slot is not
// mounted or is being ejected; eject waits for open sessions to end.
bool nv_usb_storage_session_begin(int slot);
void nv_usb_storage_session_end(int slot);

// fopen/fclose carrying a session; nv_sd_fopen/nv_sd_fclose route /usbN paths here.
FILE *nv_usb_storage_fopen(const char *path, const char *mode);
bool  nv_usb_storage_fclose(FILE *f, int *ret);   // false: f was not opened here

// Every device on the bus (hubs included) for diagnostics / the web API.
typedef struct {
    uint8_t  addr;
    uint8_t  parent_addr;       // 0 = root port, 0xFF = behind a hub not in the list
    uint8_t  port;              // parent hub port (1-based), 0 on the root port
    uint8_t  speed;             // 0 low, 1 full, 2 high
    uint16_t vid, pid;
    uint8_t  dev_class;         // bDeviceClass (0 = per interface)
    uint8_t  if_classes[6];     // first interface classes (0xFF-terminated when fewer)
    char     manufacturer[24];
    char     product[32];
} nv_usb_bus_dev_t;
int nv_usb_bus_list(nv_usb_bus_dev_t *out, int max);

#ifdef __cplusplus
}
#endif
