// NucleoOS Recovery — the small app (ota_1 slot) that installs system updates staged on the microSD card
// and brings the system back when it does not start. See docs/OTA.md for the whole design.
//
// Decision on every boot (the bootloader starts us when NucleoOS asked for it, or when a system on
// probation died and the bootloader gave up on it):
//   0. NucleoOS asked for a rescue (crash loop after confirmation) -> restore the safety copy.
//   1. nvupd/next.jsn present            -> save a safety copy of the running system into `assets`
//                                           (LKG, compressed), install next.bin into `system`
//                                           (verified before, during and after the write), boot it.
//                                           A power cut mid-write lands here again and starts over.
//   2. system ABORTED (died on probation) -> if the reset was not its fault (power cut, brownout, a
//                                           deliberate restart) boot it again, at most kMaxRetries
//                                           times; otherwise restore the safety copy.
//   3. system INVALID / image broken      -> restore: LKG in flash first, then nvupd/prev.bin.
//   4. system fine                        -> boot it (first boot after the web flasher, or the card
//                                           was removed after an update was armed).
//   5. nothing bootable                   -> repair screen: a signed nucleos-anima.bin/.json in the
//                                           card's root is installed as soon as it appears.
// Recovery that crashes three times in a row (a card that hangs the FAT driver) stops touching the
// card and works from flash alone.
#include "rec_disp.h"
#include "nv_fwup.h"

#include "driver/sdmmc_host.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
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

using nv_fwup_policy::Reset;

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
bool s_limp = false;   // recovery itself kept crashing: no SD card this time
nv_fwup_journal_t s_j;

// Survives our own crashes and watchdog resets (not a power cut): were we in the middle of a run?
struct Guard { uint32_t magic, running, deaths, check; };
RTC_NOINIT_ATTR Guard s_guard;
constexpr uint32_t kGuardMagic = 0x52435652;   // "RCVR"
bool guard_ok(void) { return s_guard.magic == kGuardMagic && s_guard.check == (s_guard.running ^ s_guard.deaths ^ kGuardMagic); }
void guard_set(uint32_t running, uint32_t deaths) {
    s_guard.magic = kGuardMagic; s_guard.running = running; s_guard.deaths = deaths;
    s_guard.check = running ^ deaths ^ kGuardMagic;
}

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

void screen_on(void) {
    if (s_screen) return;
    s_screen = rec_disp_init();
    frame();
    rec_disp_backlight(80);
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
    if (s_limp) return false;
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

// ------------------------------------------------------------------ results
// What happened, for NucleoOS to tell the user once: journal in flash (always) + result.jsn on the
// card (what systems before the journal read).
void result(const char *op, bool ok, const char *from, const char *to, const char *why) {
    using nv_fwup_policy::set_field;
    s_j.res_set = 1;
    s_j.res_ok = ok;
    set_field(s_j.res_op, sizeof s_j.res_op, op);
    set_field(s_j.res_from, sizeof s_j.res_from, from);
    set_field(s_j.res_to, sizeof s_j.res_to, to);
    set_field(s_j.res_why, sizeof s_j.res_why, why);
    if (!nv_fwup_journal_store(&s_j)) ESP_LOGE(TAG, "journal write failed");
    if (s_sd) nv_fwup_result_write(kMount, op, ok, from, to, why);
    ESP_LOGI(TAG, "%s %s: %s -> %s%s%s", op, ok ? "ok" : "FAILED", from, to, why ? ": " : "", why ? why : "");
}

// ------------------------------------------------------------------ system slot
struct SysInfo {
    bool image_ok;            // verifies as an ESP app image
    bool state_bad;           // otadata: ABORTED / INVALID
    bool aborted;             // ABORTED (died on probation)
    bool confirmed;           // VALID, or no state at all (fresh USB flash): a proven system
    uint32_t len;
    char ver[32];
};

SysInfo sys_info(const esp_partition_t *sys) {
    SysInfo s = {};
    s.image_ok = nv_fwup_image_info(sys, &s.len, s.ver, sizeof s.ver);
    if (!s.image_ok) snprintf(s.ver, sizeof s.ver, "?");
    esp_ota_img_states_t st;
    const bool have_state = nv_fwup_slot_state(sys, &st) == ESP_OK;   // newest entry, as the bootloader
    s.aborted = have_state && st == ESP_OTA_IMG_ABORTED;
    s.state_bad = have_state && (st == ESP_OTA_IMG_ABORTED || st == ESP_OTA_IMG_INVALID);
    s.confirmed = s.image_ok && (!have_state || st == ESP_OTA_IMG_VALID || st == ESP_OTA_IMG_UNDEFINED);
    return s;
}

[[noreturn]] void boot_system(const esp_partition_t *sys, const char *it, const char *en) {
    // set_boot_partition marks the image NEW: it boots on probation again (nv_fwup_policy::probation).
    if (esp_ota_set_boot_partition(sys) != ESP_OK) ESP_LOGE(TAG, "set boot -> system failed");
    if (it && s_screen) {
        show("NucleoOS", it, en, kOk, -1);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    guard_set(0, 0);   // a clean exit
    esp_restart();
}

// The running system goes into the LKG area before anything replaces it. Only a confirmed system:
// never copy an image that is itself on probation or was given up on.
bool save_safety_copy(const esp_partition_t *sys, const SysInfo &si) {
    nv_fwup_lkg_info_t li;
    if (!si.confirmed) return nv_fwup_lkg_info(&li);
    Job j = { "Copia di sicurezza", "Salvataggio della versione attuale...", "Saving a safety copy of this version...", -1 };
    const nv_fwup_err_t e = nv_fwup_lkg_save(sys, si.len, si.ver, progress, &j);
    if (e != NV_FWUP_OK) ESP_LOGW(TAG, "no safety copy of v%s: %s", si.ver, nv_fwup_err_str(e));
    return e == NV_FWUP_OK;
}

bool prev_on_card(const esp_partition_t *sys, nv_fwup_manifest_t *m) {
    char man[64], bin[64];
    return s_sd && exists(path(bin, NV_FWUP_PREV_BIN)) &&
           nv_fwup_manifest_load(path(man, NV_FWUP_PREV_MAN), sys->size, m) == NV_FWUP_OK;
}

// Restore the last good system. `failed` = the version that does not work (a copy of that same
// version is used only when the slot itself is damaged). Returns only on failure.
void restore(const esp_partition_t *sys, const SysInfo &si, const char *op, const char *why) {
    const bool damaged = !si.image_ok;
    char done_to[32] = "";

    // 1. LKG in flash: no card needed.
    nv_fwup_lkg_info_t li;
    if (nv_fwup_lkg_info(&li) && (damaged || strcmp(li.version, si.ver))) {
        Job j = { "Ripristino", "Ripristino della versione precedente...", "Restoring the previous version...", -1 };
        if (nv_fwup_lkg_restore(sys, progress, &j) == NV_FWUP_OK) snprintf(done_to, sizeof done_to, "%s", li.version);
    }
    // 2. The copy NucleoOS saved on the card before updating.
    nv_fwup_manifest_t m;
    if (!done_to[0] && prev_on_card(sys, &m) && (damaged || strcmp(m.version, si.ver))) {
        char bin[64];
        Job j = { "Ripristino", "Ripristino della versione precedente...", "Restoring the previous version...", -1 };
        if (nv_fwup_install(path(bin, NV_FWUP_PREV_BIN), &m, sys, progress, &j) == NV_FWUP_OK)
            snprintf(done_to, sizeof done_to, "%s", m.version);
    }
    if (!done_to[0]) {
        ESP_LOGE(TAG, "%s: nothing to restore v%s from", op, si.ver);
        if (!strcmp(op, "rescue")) nv_fwup_policy::set_field(s_j.no_rescue_ver, sizeof s_j.no_rescue_ver, si.ver);
        result(op, false, si.ver, "", "no safety copy to restore");
        return;
    }
    // The signed manifest of what is now installed, when the card has it (rollback copy next time).
    if (s_sd) {
        char cur[64], pm[64];
        path(cur, NV_FWUP_CUR_MAN);
        if (nv_fwup_manifest_load(path(pm, NV_FWUP_PREV_MAN), sys->size, &m) == NV_FWUP_OK && !strcmp(m.version, done_to))
            nv_fwup_manifest_save(cur, &m);
        else
            remove(cur);   // unknown: NucleoOS fetches its own manifest again
    }
    nv_fwup_policy::set_field(s_j.retry_ver, sizeof s_j.retry_ver, done_to);
    s_j.retries = 0;
    result(op, true, si.ver, done_to, why);
    boot_system(sys, "Versione precedente ripristinata", "Previous version restored");
}

// Pending update in nvupd/. Returns only when the update could not be installed.
void run_update(const esp_partition_t *sys, const SysInfo &si) {
    char bin[64], man[64], cur[64];
    path(bin, NV_FWUP_NEXT_BIN);
    path(man, NV_FWUP_NEXT_MAN);
    nv_fwup_manifest_t m;
    const nv_fwup_err_t me = nv_fwup_manifest_load(man, sys->size, &m);
    const int tries = nv_fwup_tries_get(kMount);
    const char *why = nullptr;
    if (me != NV_FWUP_OK) why = nv_fwup_err_str(me);
    else if (tries >= NV_FWUP_MAX_TRIES) why = "too many failed attempts";
    if (why) {
        ESP_LOGE(TAG, "pending update discarded: %s", why);
        result("install", false, si.ver, me == NV_FWUP_OK ? m.version : "?", why);
        remove(man); remove(bin); nv_fwup_tries_set(kMount, 0);
        return;
    }
    nv_fwup_tries_set(kMount, tries + 1);   // counted before writing: a crash mid-write counts too

    // Safety copy first (skipped quickly when the LKG already holds this exact system).
    const bool have_copy = save_safety_copy(sys, si);
    nv_fwup_manifest_t pm;
    const bool have_prev = prev_on_card(sys, &pm) && strcmp(pm.version, m.version);
    if ((s_j.install_flags & nv_fwup_policy::kInstallNeedSafetyCopy) && !have_copy && !have_prev && si.confirmed) {
        // A hands-free update must leave a way back; this one would not. Keep the working system.
        result("install", false, si.ver, m.version, "no safety copy");
        remove(man); remove(bin); nv_fwup_tries_set(kMount, 0);
        return;
    }

    char title[48];
    snprintf(title, sizeof title, "NucleoOS %s", m.version);
    Job j = { title, "Installazione dell'aggiornamento...", "Installing the update...", -1 };
    const nv_fwup_err_t err = nv_fwup_install(bin, &m, sys, progress, &j);
    ESP_LOGI(TAG, "install v%s from %s: %s", m.version, bin, nv_fwup_err_str(err));
    if (err == NV_FWUP_OK) {
        path(cur, NV_FWUP_CUR_MAN);
        remove(cur);                                // FAT rename never replaces an existing file
        if (rename(man, cur) != 0) {                // the signed manifest of what is now installed
            nv_fwup_manifest_save(cur, &m);
            remove(man);
        }
        remove(bin);
        nv_fwup_tries_set(kMount, 0);
        nv_fwup_policy::set_field(s_j.retry_ver, sizeof s_j.retry_ver, m.version);
        s_j.retries = 0;
        s_j.install_flags = 0;
        result("install", true, si.ver, m.version, nullptr);
        boot_system(sys, "Aggiornamento completato", "Update complete");
    }
    result("install", false, si.ver, m.version, nv_fwup_err_str(err));
    // A bad file is refused before the slot is erased and is never tried again; an I/O error (a card
    // that hiccupped) is retried on the next boot until NV_FWUP_MAX_TRIES.
    if (err != NV_FWUP_E_IO && err != NV_FWUP_E_NOMEM) {
        remove(man); remove(bin); nv_fwup_tries_set(kMount, 0);
    }
}

// Signed firmware in the card's root: the offline repair path (release assets nucleos-anima.bin/.json).
bool try_root_image(const esp_partition_t *sys) {
    if (!s_sd) return false;
    char bin[64], man[64];
    snprintf(bin, sizeof bin, "%s/nucleos-anima.bin", kMount);
    snprintf(man, sizeof man, "%s/nucleos-anima.json", kMount);
    nv_fwup_manifest_t m;
    if (!exists(bin) || nv_fwup_manifest_load(man, sys->size, &m) != NV_FWUP_OK) return false;
    Job j = { "Riparazione", "Installazione di NucleoOS dalla microSD...", "Installing NucleoOS from the microSD card...", -1 };
    if (nv_fwup_install(bin, &m, sys, progress, &j) == NV_FWUP_OK) {
        char cur[64];
        nv_fwup_manifest_save(path(cur, NV_FWUP_CUR_MAN), &m);
        result("restore", true, "", m.version, nullptr);
        boot_system(sys, "NucleoOS installato", "NucleoOS installed");
    }
    return false;
}

[[noreturn]] void repair_screen(const esp_partition_t *sys) {
    screen_on();
    show("NucleoOS non si avvia",
         "Copia nucleos-anima.bin e .json dalla release sulla microSD",
         "Copy nucleos-anima.bin and .json from the release to the microSD card", kWarn, -1);
    rec_text_center(&rec_font_small, REC_W / 2, kCardY + 196,
                    "oppure / or: indecenti.github.io/nucleoos-p4-store/flash", kAccent);
    rec_flush();
    guard_set(0, 0);   // waiting here is not a crash
    for (;;) {   // wait for a card with a usable image
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (s_limp) continue;
        if (!s_sd) s_sd = sd_mount();
        if (s_sd) {
            const SysInfo si = sys_info(sys);
            if (si.image_ok && !si.state_bad) boot_system(sys, nullptr, nullptr);
            try_root_image(sys);
        }
    }
}

}  // namespace

extern "C" void app_main(void) {
    const esp_app_desc_t *self = esp_app_get_description();
    const esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "NucleoOS Recovery %s (reset reason %d)", self->version, (int)rr);
    // We are the ota_1 slot, so the bootloader starts us PENDING_VERIFY like any fresh OTA image.
    // Recovery never changes in the field: confirm at once, or a reset during a long install would
    // make the bootloader "roll back" from recovery to a half-written system.
    esp_ota_mark_app_valid_cancel_rollback();

    // Did a previous run of ours die midway? Three times in a row: leave the card alone.
    uint32_t deaths = (guard_ok() && s_guard.running) ? s_guard.deaths + 1 : 0;
    if (rr == ESP_RST_POWERON) deaths = 0;
    s_limp = deaths >= 3;
    guard_set(1, deaths);
    if (s_limp) ESP_LOGE(TAG, "recovery died %lu times in a row: working without the SD card", (unsigned long)deaths);

    const esp_partition_t *sys = nv_fwup_system_part();
    if (!sys) { ESP_LOGE(TAG, "no system partition - wrong partition table"); for (;;) vTaskDelay(portMAX_DELAY); }

    nv_fwup_journal_load(&s_j);
    s_sd = sd_mount();
    char next_man[64];
    const bool pending = s_sd && exists(nv_fwup_path(next_man, sizeof next_man, kMount, NV_FWUP_NEXT_MAN));
    SysInfo si = sys_info(sys);
    const bool rescue = s_j.rescue && si.image_ok && !strcmp(s_j.rescue_ver, si.ver);
    ESP_LOGI(TAG, "system v%s image=%d confirmed=%d aborted=%d bad=%d pending=%d rescue=%d sd=%d",
             si.ver, si.image_ok, si.confirmed, si.aborted, si.state_bad, pending, rescue, s_sd);
    if (s_j.rescue && !rescue) {   // a stale request (the system changed since): drop it
        s_j.rescue = 0;
        nv_fwup_journal_store(&s_j);
    }

    // Nothing to do (fresh flash, or the card left after an update was armed): straight to NucleoOS,
    // without even lighting the screen.
    if (!pending && !rescue && si.image_ok && !si.state_bad) boot_system(sys, nullptr, nullptr);

    screen_on();
    show("NucleoOS", "Avvio del ripristino...", "Starting recovery...", kText, -1);

    // 0. NucleoOS kept crashing after it had been confirmed, and asked for the safety copy.
    if (rescue) {
        char why[48];
        snprintf(why, sizeof why, "%s", s_j.rescue_why);
        s_j.rescue = 0;
        nv_fwup_journal_store(&s_j);
        if (pending) {   // an update is waiting: it may well be the fix
            run_update(sys, si);
            si = sys_info(sys);
        }
        restore(sys, si, "rescue", why[0] ? why : "repeated crashes");
        // Nothing to restore from: keep the system (its safe mode can still take an update). Re-read
        // the slot: a restore that failed midway may have left it damaged.
        si = sys_info(sys);
        if (si.image_ok && !si.state_bad) boot_system(sys, nullptr, nullptr);
    }

    // 1. An update is waiting.
    if (pending) {
        run_update(sys, si);
        si = sys_info(sys);
        if (si.image_ok && !si.state_bad) boot_system(sys, nullptr, nullptr);   // refused; old system intact
    }

    // 2. Died on probation: its fault, or a power cut?
    if (si.aborted && si.image_ok) {
        const int done = strcmp(s_j.retry_ver, si.ver) ? 0 : (int)s_j.retries;
        if (nv_fwup_policy::on_aborted(nv_fwup_reset_kind(rr), done) == nv_fwup_policy::AbortAction::Retry) {
            nv_fwup_policy::set_field(s_j.retry_ver, sizeof s_j.retry_ver, si.ver);
            s_j.retries = (uint32_t)done + 1;
            nv_fwup_journal_store(&s_j);
            ESP_LOGW(TAG, "v%s stopped by a reset that was not its fault (%d): retry %d/%d", si.ver, (int)rr,
                     done + 1, nv_fwup_policy::kMaxRetries);
            boot_system(sys, nullptr, nullptr);
        }
    }

    // 3. The system does not start: bring the previous one back.
    ESP_LOGW(TAG, "system v%s does not start: restoring", si.ver);
    // The system may have said why it gave up on itself (probation: UI frozen, server unreachable).
    const char *why = !si.image_ok ? "damaged image"
                    : (!strcmp(s_j.rescue_ver, si.ver) && s_j.rescue_why[0]) ? s_j.rescue_why : "did not start";
    char why_copy[48];
    snprintf(why_copy, sizeof why_copy, "%s", why);
    restore(sys, si, "rollback", why_copy);
    try_root_image(sys);
    repair_screen(sys);
}
