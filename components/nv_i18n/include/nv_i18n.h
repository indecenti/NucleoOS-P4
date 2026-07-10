// nv_i18n — NucleoOS Anima internationalization layer.
// A tiny compile-time string table: every UI string is an id (nv_str_id_t); nv_tr() maps
// the id to the active language's text, falling back to English when a cell is missing.
// The active language is persisted to nv_config ("lang") and a change publishes
// NV_EV_LANG_CHANGED so SystemUI can re-render live. No heap, no runtime parsing.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Supported languages (index into the string table). Keep NV_LANG_EN first (the fallback).
typedef enum {
    NV_LANG_EN = 0,
    NV_LANG_IT,
    NV_LANG_ES,
    NV_LANG_FR,
    NV_LANG_DE,
    NV_LANG_COUNT
} nv_lang_t;

// Catalog ids — one per translatable UI string. Order MUST match the table in nv_i18n.c.
typedef enum {
    NV_STR_APP_SETTINGS = 0,
    NV_STR_APP_FILES,
    NV_STR_APP_ANIMA,
    NV_STR_APP_DIAG,
    NV_STR_APP_TERMINAL,
    NV_STR_APP_GALLERY,
    NV_STR_APP_MUSIC,
    NV_STR_APP_CAMERA,
    NV_STR_APP_VIDEO,
    NV_STR_APP_CALC,
    NV_STR_APP_TASKS,
    NV_STR_BACK,
    NV_STR_CLOSE,
    NV_STR_COMING_SOON,
    NV_STR_APP_COMING_SOON,
    NV_STR_QUICK_SETTINGS,
    NV_STR_NO_NOTIFICATIONS,
    NV_STR_MUTE,
    NV_STR_SET_NETWORK,
    NV_STR_SET_DISPLAY,
    NV_STR_SET_SOUND,
    NV_STR_SET_STORAGE,
    NV_STR_SET_MEMORY,
    NV_STR_SET_ANIMA,
    NV_STR_SET_LANGUAGE,
    NV_STR_SET_ABOUT,
    NV_STR_BRIGHTNESS,
    NV_STR_DARK_THEME,
    NV_STR_VOLUME,
    NV_STR_DUMP_LOG,
    NV_STR_LANGUAGE,
    NV_STR_STORAGE_INFO,
    NV_STR_MEM_STATS,
    NV_STR_ABOUT_INFO,
    NV_STR_PHOTOS,
    NV_STR_ITEMS_FMT,
    NV_STR_SCIENTIFIC,
    NV_STR_STANDARD,
    NV_STR_THEME,
    NV_STR_LIGHT,
    NV_STR_DARK,
    NV_STR_ACCENT_COLOR,
    NV_STR_FONT_SIZE,
    NV_STR_FONT_NORMAL,
    NV_STR_FONT_LARGE,
    NV_STR_NO_PHOTOS,
    NV_STR_ADD_PHOTOS_HINT,
    NV_STR_SD_MISSING,
    NV_STR_IMAGE_UNAVAILABLE,
    NV_STR_APP_NOTES,
    NV_STR_NOTES_PLACEHOLDER,
    NV_STR_SAVE,
    NV_STR_SAVED,
    NV_STR_SAVE_FAILED,
    NV_STR_NOTES_NEW,        // Notes app: new-note button
    NV_STR_NOTES_TITLE_PH,   // Notes app: title field placeholder
    NV_STR_NOTES_EMPTY,      // Notes app: empty state
    NV_STR_NOTES_SAVED_FMT,  // Notes app: "Saved at %s" (autosave status)
    NV_STR_NOTES_SAVING,     // Notes app: autosave in progress
    NV_STR_NOTES_RETRY,      // Notes app: retry a failed save
    NV_STR_NOTES_DISCARD,    // Notes app: discard unsaved edits and leave
    NV_STR_WIFI,
    NV_STR_WIFI_OFF,
    NV_STR_WIFI_DEMO,
    NV_STR_WIFI_CONNECTED,
    NV_STR_WIFI_ONLINE,
    NV_STR_WIFI_CONNECTING,
    NV_STR_WIFI_SCANNING,
    NV_STR_WIFI_SCAN,
    NV_STR_WIFI_DISCONNECT,
    NV_STR_WIFI_FORGET,
    NV_STR_WIFI_CONNECT,
    NV_STR_WIFI_FAILED,
    NV_STR_WIFI_AVAILABLE,
    NV_STR_WIFI_NO_NETWORKS,
    NV_STR_WIFI_SAVED,
    NV_STR_WIFI_OPEN,
    NV_STR_WIFI_PASSWORD,
    NV_STR_WIFI_SHOW_PASSWORD,
    NV_STR_CANCEL,
    NV_STR_SET_UPDATE,
    NV_STR_UPDATE_CURRENT,
    NV_STR_UPDATE_CHECK,
    NV_STR_UPDATE_INSTALL,
    NV_STR_UPDATE_RESTART,
    NV_STR_UPDATE_URL,
    NV_STR_UPDATE_FROM_SD,
    NV_STR_SET_BACKUP,
    NV_STR_BACKUP_INFO,
    NV_STR_BACKUP_NOW,
    NV_STR_BACKUP_RESTORE,
    // ---- Settings v2 (split view + new pages) ----
    NV_STR_SET_DATETIME,     // "Date & time" category
    NV_STR_TIME_24H,         // 24-hour format switch
    NV_STR_TIME_SYNCED,      // clock is NTP-synced
    NV_STR_TIME_WAIT_SYNC,   // waiting for first NTP sync
    NV_STR_TIMEZONE,         // section: time zone list
    NV_STR_SCREEN_SLEEP,     // display: idle screen-off
    NV_STR_SLEEP_NEVER,      // screen sleep "Never" choice
    NV_STR_KEY_CLICK,        // keyboard click sounds switch
    NV_STR_STARTUP_CHIME,    // boot chime switch
    NV_STR_TEST_SOUND,       // play test sound button
    NV_STR_MUTED,            // sound subtitle when muted
    NV_STR_STORAGE_SD,       // "microSD card" section
    NV_STR_STORAGE_FLASH,    // "Internal flash" section
    NV_STR_MB_FREE_OF,       // "%u MB free of %u MB"
    NV_STR_SD_HINT,          // insert-a-card hint
    NV_STR_NVS_USAGE,        // "Preferences store: %u of %u entries"
    NV_STR_KB_FREE,          // "%u KB free"
    NV_STR_MB_FREE,          // "%u MB free"
    NV_STR_GROUP_CONNECT,    // rail group: Connectivity
    NV_STR_GROUP_DEVICE,     // rail group: Device
    NV_STR_GROUP_PERSONAL,   // rail group: Personalization
    NV_STR_GROUP_SYSTEM,     // rail group: System
    NV_STR_ANIMA_TAGLINE,    // hero tagline
    NV_STR_ANIMA_DESC,       // what Anima will be
    NV_STR_ANIMA_SOON,       // not active yet
    NV_STR_ABOUT_VERSION,    // kv label
    NV_STR_ABOUT_BUILD,      // kv label: build date
    NV_STR_ABOUT_UPTIME,     // kv label
    NV_STR_UPTIME_FMT,       // "%ud %uh %um" (localized unit letters)
    NV_STR_RESTART_DEVICE,   // about: restart button
    NV_STR_FACTORY_RESET,    // backup page: danger section
    NV_STR_FACTORY_INFO,     // what a reset erases
    NV_STR_ERASE_CONFIRM,    // modal title
    NV_STR_ERASE_BTN,        // modal confirm button
    NV_STR_ACC_BLUE,         // accent color names
    NV_STR_ACC_GREEN,
    NV_STR_ACC_PURPLE,
    NV_STR_ACC_ORANGE,
    // ---- notification center + quick settings v2 ----
    NV_STR_NOTIFICATIONS,    // shade section header
    NV_STR_CLEAR_ALL,        // shade: clear notification list
    NV_STR_UPDATED_TO,       // "Updated to %s" (post-OTA boot notification)
    NV_STR_RESET_FAILED,     // factory reset aborted: SD backup could not be removed
    NV_STR_ETH_DOWN,         // ethernet: no cable / no link
    NV_STR_TEMPERATURE,      // about: on-die temperature row
    // ---- lock screen + PIN ----
    NV_STR_SCREEN_LOCK,      // display: enable idle lock
    NV_STR_LOCK_NOW,         // quick-settings action
    NV_STR_UNLOCK,           // lock screen button (no PIN)
    NV_STR_ENTER_PIN,        // lock screen prompt
    NV_STR_WRONG_PIN,        // lock screen error
    NV_STR_SET_PIN,          // settings: define/replace the unlock PIN
    NV_STR_REMOVE_PIN,       // settings: clear the PIN
    NV_STR_PIN_SAVED,        // toast after storing a PIN
    NV_STR_CONFIRM_PIN,      // set flow: re-enter to confirm
    NV_STR_LOCK_ON_BOOT,     // require the PIN at startup
    NV_STR_SEARCH,           // launcher search placeholder
    NV_STR_NO_RESULTS,       // launcher search: nothing matched
    NV_STR_SET_SENSORS,      // rail: Sensors category
    NV_STR_SET_ACCESS,       // rail: Accessibility category
    NV_STR_APP_APPS,         // launcher: WASM app manager
    NV_STR_RUN,              // run a WASM app
    NV_STR_NO_APPS,          // app manager empty state
    NV_STR_I2C_DEVICES,      // sensors: I2C bus scan section
    NV_STR_RESCAN,           // sensors: re-run the I2C scan
    NV_STR_DND,              // notifications: Do Not Disturb
    NV_STR_NONE,             // generic "None"/empty result
    NV_STR_RUNNING,          // WASM runner: app executing
    NV_STR_STOP,             // WASM runner: abort the running app
    NV_STR_WASM_BUSY,        // WASM runner: another run is still active
    NV_STR_WASM_TIMEOUT,     // WASM runner: watchdog stopped the app
    NV_STR_WASM_OK_FMT,      // WASM runner: success status ("OK - %u ms")
    NV_STR_APPS_INSTALLED_FMT, // app manager header ("%d apps installed")
    NV_STR_CRASH_NOTIF_FMT,  // boot notification ("Last boot crashed: %s @ 0x%08x")
    NV_STR_LAST_CRASH,       // diagnostics: crash section title
    NV_STR_NO_CRASH,         // diagnostics: no stored crash
    NV_STR_CRASH_INFO_FMT,   // diagnostics: crash details ("Task %s @ PC 0x%08x - dump %u KB")
    NV_STR_CLEAR,            // generic clear/erase action
    NV_STR_RENAME,           // files: rename action
    NV_STR_DELETE,           // files: delete action
    NV_STR_TAP_AGAIN,        // files: two-step delete confirm
    NV_STR_OPEN,             // files: open (image viewer)
    NV_STR_MICROPHONE,       // sound: mic section title
    NV_STR_MIC_TEST,         // sound: record-and-playback test button
    NV_STR_RECORDING,        // sound: mic test recording state
    NV_STR_PLAYING,          // sound: mic test playback state
    NV_STR_MIC_MISSING,      // sound: ES7210 unavailable
    // ---- Second Screen (USB extended display) ----
    NV_STR_APP_SCREEN,       // launcher label
    NV_STR_SS_WAIT,          // waiting for PC frames
    NV_STR_SS_HINT,          // cable + driver instructions
    NV_STR_SS_USB_OK,        // USB cable connected (host configured us)
    NV_STR_SS_USB_NO,        // USB cable not connected
    NV_STR_SS_PAUSED,        // streaming paused after edge-swipe exit
    NV_STR_SS_RESUME,        // resume streaming button
    NV_STR_ROTATE,           // quick-settings chip: portrait/landscape toggle
    NV_STR_FOLDER,           // launcher: default folder name
    NV_STR_SS_AUTO,          // Second Screen: auto-open app when the PC starts streaming
    NV_STR_SS_PC_CONNECTED,  // Second Screen: toast/notification on PC display link
    NV_STR_SCREENSHOT,       // quick-settings action chip
    NV_STR_SHOT_SAVED,       // notification: screenshot written to SD
    NV_STR_SHOT_FAIL,        // notification: screenshot capture failed
    NV_STR_RECENTS,          // task switcher overlay title
    NV_STR_NO_RECENTS,       // task switcher empty state
    NV_STR_SET_SECURITY,     // Settings category: Security
    NV_STR_ENCRYPTION,       // security page: encryption status label
    NV_STR_ENC_OFF,          // security page: encryption disabled (plaintext secrets)
    NV_STR_TASK_ADD,         // Tasks app: add button
    NV_STR_TASK_NEW,         // Tasks app: new-task input placeholder
    NV_STR_NO_TASKS,         // Tasks app: empty state
    NV_STR_NO_CAMERA,        // Camera app: no sensor detected
    NV_STR_CAPTURE,          // Camera app: shutter button
    NV_STR_PHOTO_SAVED,      // Camera app: photo written to SD
    NV_STR_NO_MUSIC,         // Music app: empty state
    NV_STR_NO_VIDEO,         // Video app: empty state
    NV_STR_APP_RECORDER,     // Voice Recorder app name
    NV_STR_REC_EMPTY,        // Recorder: no recordings yet
    NV_STR_NO_MIC,           // Recorder: microphone unavailable
    // ---- System Monitor (task manager) ----
    NV_STR_APP_SYSMON,       // launcher label
    NV_STR_SM_PERF,          // tab: Performance
    NV_STR_SM_PROC,          // tab: Processes
    NV_STR_SM_SERVICES,      // tab: Services
    NV_STR_SM_SYSTEM,        // perf card: System
    NV_STR_SM_INTERNAL,      // perf card: Internal RAM (SRAM)
    NV_STR_SM_FREQ,          // perf: CPU frequency label
    NV_STR_SM_UPTIME,        // perf: uptime label
    NV_STR_SM_STACK,         // processes column: stack free
    NV_STR_SM_STATE,         // processes column: state
    NV_STR_SM_CORE,          // processes column: core affinity
    NV_STR_SM_PRIO,          // processes column: priority
    NV_STR_SM_STOPPED,       // state: stopped
    NV_STR_SM_READY,         // task state: ready
    NV_STR_SM_BLOCKED,       // task state: blocked
    NV_STR_SM_SUSPENDED,     // task/service state: suspended
    NV_STR_SM_ESSENTIAL,     // service badge: essential
    NV_STR_SM_LARGEST,       // memory: largest free block
    NV_STR_GENERATING_THUMBS, // Gallery: first-run thumbnail-cache backlog toast
    // ---- App Store (remote WASM app catalog) ----
    NV_STR_STORE_STORE,        // tab: Store
    NV_STR_STORE_INSTALLED,    // tab / status: Installed
    NV_STR_STORE_INSTALL,      // button: Install
    NV_STR_STORE_UPDATE,       // button: Update
    NV_STR_STORE_INSTALLING,   // status: Installing…
    NV_STR_STORE_UPDATE_AVAIL, // status: Update available
    NV_STR_STORE_NOT_INSTALLED,// status: Not installed
    NV_STR_STORE_ALL,          // category filter: All
    NV_STR_STORE_FEATURED,     // category filter / badge: Featured
    NV_STR_STORE_CONTACTING,   // status: Contacting store…
    NV_STR_STORE_EMPTY,        // empty state: no apps for this region
    NV_STR_STORE_REFRESH,      // button: Refresh
    NV_STR_STORE_RETRY,        // button: Retry
    NV_STR_STORE_NEEDS_OS,     // status: app needs a newer OS (ABI)
    NV_STR_STORE_SOURCE,       // label: Source
    NV_STR_STORE_REGION,       // label: Region
    NV_STR_STORE_NO_SD,        // empty state: insert SD to install
    NV_STR_COUNT
} nv_str_id_t;

// Load the saved language from nv_config ("lang", default NV_LANG_EN), clamped to range.
void nv_i18n_init(void);

// Set the active language. No-op if unchanged; otherwise persists it and publishes
// NV_EV_LANG_CHANGED (payload: const nv_lang_t*).
void nv_i18n_set_lang(nv_lang_t l);

// The active language.
nv_lang_t nv_i18n_get_lang(void);

// Translated string for `id` in the active language. Falls back to English when the cell
// is missing/empty; returns "" for an out-of-range id. Never returns NULL.
const char *nv_tr(nv_str_id_t id);

// Native display name of a language ("English", "Italiano", ...). "" if out of range.
const char *nv_lang_native_name(nv_lang_t l);

// Localized short calendar names in the active language (ASCII-safe; falls back to English).
// wday: 0=Sunday..6=Saturday (struct tm.tm_wday). mon: 0=January..11=December (tm.tm_mon).
// Static storage — never free.
const char *nv_i18n_wday_short(int wday);
const char *nv_i18n_month_short(int mon);

#ifdef __cplusplus
}
#endif
