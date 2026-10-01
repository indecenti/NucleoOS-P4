// NucleoOS Recovery — the small app (ota_1 slot) that installs system updates staged on the microSD card
// and brings the system back when an update does not start. See docs/OTA.md for the whole design.
//
// Decision on every boot (the bootloader starts us when NucleoOS asked for it, or when a new system
// failed its 60 s survival gate and was rolled back to this slot):
//   1. nvupd/next.jsn present            -> install next.bin into `system` (verified before, during
//                                           and after the write), boot it. A power cut mid-write
//                                           lands here again and simply starts over.
//   2. system image aborted / invalid    -> restore nvupd/prev.bin (the copy NucleoOS saved before
//                                           updating), boot it.
//   3. system image fine                 -> boot it (first boot after the web flasher, or the card
//                                           was removed after an update was armed).
//   4. nothing bootable                  -> repair screen: a signed nucleos-anima.bin/.json in the
//                                           card's root is installed as soon as it appears.
#include "rec_disp.h"
#include "nv_fwup.h"

#include "driver/sdmmc_host.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static const char *TAG = "recovery";

namespace {

constexpr const char *kMount = "/sdcard";

// Palette (NucleoOS dark theme).
const uint16_t kBg     = REC_RGB(0x0B, 0x0F, 0x17);
const uint16_t kCard   = REC_RGB(0x16, 0x1C, 0x28);
const uint16_t kTrack  = REC_RGB(0x26, 0x2E, 0x3D);
const uint16_t kAccent = REC_RGB(0x4F, 0x8C, 0xFF);
const uint16_t kOk     = REC_RGB(0x3D, 0xD6, 0x8C);
const uint16_t kWarn   = REC_RGB(0xFF, 0xB0, 0x3B);
const uint16_t kText   = REC_RGB(0xEC, 0xF0, 0xF6);
const uint16_t kDim    = REC_RGB(0x8A, 0x94, 0xA6);

bool s_screen = false;
bool s_sd = false;

// ------------------------------------------------------------------ screen
constexpr int kCardX = 152, kCardY = 196, kCardW = 720, kCardH = 300;
constexpr int kBarX = kCardX + 48, kBarY = kCardY + 168, kBarW = kCardW - 96, kBarH = 14;

void frame(void) {
    if (!s_screen) return;
    rec_fill(0, 0, REC_W, REC_H, kBg);
    rec_image(&rec_logo, (REC_W - rec_logo.w) / 2, 40, kBg);
    rec_text_center(&rec_font_small, REC_W / 2, 160, "NucleoOS Recovery", kDim);
    rec_round_rect(kCardX, kCardY, kCardW, kCardH, 22, kCard);
    rec_text_center(&rec_font_small, REC_W / 2, REC_H - 48,
                    "Non spegnere il dispositivo  \xC2\xB7  Do not power off", kDim);
}

// Title + two lines (Italian, English) in the card; optional progress bar.
void show(const char *title, const char *it, const char *en, uint16_t title_c, int pct) {
    if (!s_screen) return;
    rec_fill(kCardX + 24, kCardY + 20, kCardW - 48, kCardH - 40, kCard);
    rec_text_center(&rec_font_title, REC_W / 2, kCardY + 36, title, title_c);
    if (it) rec_text_center(&rec_font_body, REC_W / 2, kCardY + 100, it, kText);
    if (en) rec_text_center(&rec_font_small, REC_W / 2, kCardY + 134, en, kDim);
    if (pct >= 0) {
        rec_round_rect(kBarX, kBarY, kBarW, kBarH, kBarH / 2, kTrack);
        const int w = kBarW * pct / 100;
        if (w >= kBarH) rec_round_rect(kBarX, kBarY, w, kBarH, kBarH / 2, kAccent);
        char p[16];
        snprintf(p, sizeof p, "%d%%", pct < 0 ? 0 : pct > 100 ? 100 : pct);
        rec_text_center(&rec_font_body, REC_W / 2, kBarY + 34, p, kText);
    }
    rec_flush();
}

struct Job { const char *title, *it, *en; int last; };
void progress(int pct, void *u) {
    Job *j = (Job *)u;
    if (pct == j->last) return;
    j->last = pct;
    show(j->title, j->it, j->en, kText, pct);
}

// ------------------------------------------------------------------ SD card
// Same wiring as components/nv_hal/nv_sd.cpp (slot 0, 4-bit, LDO VO4 3.3 V).
bool sd_mount(void) {
    static esp_ldo_channel_handle_t ldo = nullptr;
    if (!ldo) {
        esp_ldo_channel_config_t c = {};
        c.chan_id = 4;
        c.voltage_mv = 3300;
        esp_ldo_acquire_channel(&c, &ldo);
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;
    slot.d1 = GPIO_NUM_40;
    slot.d2 = GPIO_NUM_41;
    slot.d3 = GPIO_NUM_42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t m = {};
    m.format_if_mount_failed = false;   // never format a user's card
    m.max_files = 4;
    m.allocation_unit_size = 16 * 1024;
    sdmmc_card_t *card = nullptr;
    esp_err_t e = esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &m, &card);
    if (e != ESP_OK) {
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        e = esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &m, &card);
    }
    return e == ESP_OK;
}

bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

char *path(char *buf, const char *name) { return nv_fwup_path(buf, 64, kMount, name); }

// ------------------------------------------------------------------ system slot
bool system_valid(const esp_partition_t *sys) {
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(sys, &st) == ESP_OK &&
        (st == ESP_OTA_IMG_ABORTED || st == ESP_OTA_IMG_INVALID))
        return false;
    const esp_partition_pos_t pos = { (uint32_t)sys->address, (uint32_t)sys->size };
    esp_image_metadata_t md;
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) == ESP_OK;
}

void system_version(const esp_partition_t *sys, char *out, size_t n) {
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(sys, &d) == ESP_OK) snprintf(out, n, "%s", d.version);
    else snprintf(out, n, "?");
}

[[noreturn]] void boot_system(const esp_partition_t *sys, const char *it, const char *en) {
    if (esp_ota_set_boot_partition(sys) != ESP_OK) ESP_LOGE(TAG, "set boot -> system failed");
    if (it) {
        show("NucleoOS", it, en, kOk, -1);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    esp_restart();
}

// Install `bin` described by manifest `man` into the system slot. True on success.
bool install(const esp_partition_t *sys, const char *bin, const nv_fwup_manifest_t &m, const char *title,
             const char *it, const char *en, nv_fwup_err_t *err) {
    Job j = { title, it, en, -1 };
    *err = nv_fwup_install(bin, &m, sys, progress, &j);
    ESP_LOGI(TAG, "install v%s from %s: %s", m.version, bin, nv_fwup_err_str(*err));
    return *err == NV_FWUP_OK;
}

// Restore the copy of the previous system. Returns only on failure.
void rollback(const esp_partition_t *sys, const char *failed_ver) {
    char bin[64], man[64], res[64];
    nv_fwup_manifest_t m;
    if (!s_sd || nv_fwup_manifest_load(path(man, NV_FWUP_PREV_MAN), sys->size, &m) != NV_FWUP_OK) return;
    // result.jsn from an earlier rollback to this same version that NucleoOS never read: the copy
    // did not start either. Don't loop.
    if (FILE *f = fopen(path(res, NV_FWUP_RESULT), "rb")) {
        char b[256] = "";
        fread(b, 1, sizeof b - 1, f);
        fclose(f);
        char needle[64];
        snprintf(needle, sizeof needle, "\"to\":\"%s\"", m.version);
        if (strstr(b, "\"rollback\"") && strstr(b, needle)) { ESP_LOGE(TAG, "rollback copy failed too"); return; }
    }
    nv_fwup_err_t err;
    if (install(sys, path(bin, NV_FWUP_PREV_BIN), m, "Ripristino",
                "Ripristino della versione precedente...", "Restoring the previous version...", &err)) {
        nv_fwup_result_write(kMount, "rollback", true, failed_ver, m.version, nullptr);
        char cur[64];
        nv_fwup_manifest_save(path(cur, NV_FWUP_CUR_MAN), &m);
        boot_system(sys, "Versione precedente ripristinata", "Previous version restored");
    }
}

// Pending update in nvupd/. Returns only when there is nothing to boot.
void run_update(const esp_partition_t *sys) {
    char bin[64], man[64], cur[64];
    path(bin, NV_FWUP_NEXT_BIN);
    path(man, NV_FWUP_NEXT_MAN);
    char from[32];
    system_version(sys, from, sizeof from);
    nv_fwup_manifest_t m;
    const nv_fwup_err_t me = nv_fwup_manifest_load(man, sys->size, &m);
    const int tries = nv_fwup_tries_get(kMount);
    const char *why = nullptr;
    if (me != NV_FWUP_OK) why = nv_fwup_err_str(me);
    else if (tries >= NV_FWUP_MAX_TRIES) why = "too many failed attempts";
    if (why) {
        ESP_LOGE(TAG, "pending update discarded: %s", why);
        nv_fwup_result_write(kMount, "install", false, from, me == NV_FWUP_OK ? m.version : "?", why);
        remove(man); remove(bin); nv_fwup_tries_set(kMount, 0);
        return;
    }
    nv_fwup_tries_set(kMount, tries + 1);   // counted before writing: a crash mid-write counts too
    char title[48];
    snprintf(title, sizeof title, "NucleoOS %s", m.version);
    nv_fwup_err_t err;
    if (install(sys, bin, m, title, "Installazione dell'aggiornamento...", "Installing the update...", &err)) {
        path(cur, NV_FWUP_CUR_MAN);
        remove(cur);                                // FAT rename never replaces an existing file
        if (rename(man, cur) != 0) {                // the signed manifest of what is now installed
            nv_fwup_manifest_save(cur, &m);
            remove(man);
        }
        remove(bin);
        nv_fwup_tries_set(kMount, 0);
        nv_fwup_result_write(kMount, "install", true, from, m.version, nullptr);
        boot_system(sys, "Aggiornamento completato", "Update complete");
    }
    nv_fwup_result_write(kMount, "install", false, from, m.version, nv_fwup_err_str(err));
    // A bad file (hash/size/signature) is refused before the slot is erased; anything else may have
    // left a partial system behind. Either way this image is not tried again.
    if (err == NV_FWUP_E_HASH || err == NV_FWUP_E_SIZE || err == NV_FWUP_E_IO || err == NV_FWUP_E_VERSION ||
        err == NV_FWUP_E_IMAGE) {
        remove(man); remove(bin); nv_fwup_tries_set(kMount, 0);
    }
}

// Signed firmware in the card's root: the offline repair path (release assets nucleos-anima.bin/.json).
bool try_root_image(const esp_partition_t *sys) {
    char bin[64], man[64];
    snprintf(bin, sizeof bin, "%s/nucleos-anima.bin", kMount);
    snprintf(man, sizeof man, "%s/nucleos-anima.json", kMount);
    nv_fwup_manifest_t m;
    if (!exists(bin) || nv_fwup_manifest_load(man, sys->size, &m) != NV_FWUP_OK) return false;
    nv_fwup_err_t err;
    if (install(sys, bin, m, "Riparazione", "Installazione di NucleoOS dalla microSD...",
                "Installing NucleoOS from the microSD card...", &err)) {
        char cur[64];
        nv_fwup_manifest_save(path(cur, NV_FWUP_CUR_MAN), &m);
        nv_fwup_result_write(kMount, "restore", true, "", m.version, nullptr);
        boot_system(sys, "NucleoOS installato", "NucleoOS installed");
    }
    return false;
}

[[noreturn]] void repair_screen(const esp_partition_t *sys) {
    show("NucleoOS non si avvia",
         "Copia nucleos-anima.bin e .json dalla release sulla microSD",
         "Copy nucleos-anima.bin and .json from the release to the microSD card", kWarn, -1);
    rec_text_center(&rec_font_small, REC_W / 2, kCardY + 196,
                    "oppure / or: indecenti.github.io/nucleoos-p4-store/flash", kAccent);
    rec_flush();
    for (;;) {   // wait for a card with a usable image
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!s_sd) s_sd = sd_mount();
        if (s_sd) {
            if (system_valid(sys)) boot_system(sys, nullptr, nullptr);
            try_root_image(sys);
        }
    }
}

}  // namespace

extern "C" void app_main(void) {
    const esp_app_desc_t *self = esp_app_get_description();
    ESP_LOGI(TAG, "NucleoOS Recovery %s", self->version);
    // We are the ota_1 slot, so the bootloader starts us PENDING_VERIFY like any fresh OTA image.
    // Recovery never changes in the field: confirm at once, or a reset during a long install would
    // make the bootloader "roll back" from recovery to a half-written system.
    esp_ota_mark_app_valid_cancel_rollback();
    const esp_partition_t *sys = nv_fwup_system_part();
    if (!sys) { ESP_LOGE(TAG, "no system partition - wrong partition table"); for (;;) vTaskDelay(portMAX_DELAY); }

    s_sd = sd_mount();
    char next_man[64];
    const bool pending = s_sd && exists(nv_fwup_path(next_man, sizeof next_man, kMount, NV_FWUP_NEXT_MAN));
    const bool valid = system_valid(sys);

    // Nothing to do (fresh flash, or the card left after an update was armed): straight to NucleoOS,
    // without even lighting the screen.
    if (!pending && valid) boot_system(sys, nullptr, nullptr);

    s_screen = rec_disp_init();
    frame();
    rec_disp_backlight(80);
    show("NucleoOS", "Avvio del ripristino...", "Starting recovery...", kText, -1);

    if (pending) run_update(sys);
    if (system_valid(sys)) boot_system(sys, nullptr, nullptr);   // the update was refused; old system intact

    char ver[32];
    system_version(sys, ver, sizeof ver);
    ESP_LOGW(TAG, "system v%s does not start: restoring", ver);
    rollback(sys, ver);
    if (s_sd) try_root_image(sys);
    repair_screen(sys);
}
