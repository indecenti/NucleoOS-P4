// ANIMA's channels (OpenClaw's idea): talk to ANIMA from outside the device. Today: Telegram.
// One persistent PSRAM task polls the bot (nucleo_anima_tg_poll), lets the engine handle pairing and
// strangers (nucleo_anima_tg_accept), runs the owner's messages through the full cascade - commands
// really happen on the device - and sends the answer back. Polls every 3 s for two minutes after an
// exchange, every 15 s otherwise; idles when the channel is off, the mode is offline or there is no
// network.
#include "nv_anima_system.h"
#include "nucleo_anima.h"
#include "nv_config.h"
#include "nv_i18n.h"
#include "nv_ui.h"
#include "nv_mem_attr.h"
#include "term_sh.h"        // ANIMA's shell tool: the Terminal's shell, run headless
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace {

const char *TAG = "anima_tg";
TaskHandle_t s_task = nullptr;

// Hold the engine for one job: the user may be talking to ANIMA on the screen right now.
bool lock_engine(int wait_ms)
{
    for (int t = 0; t <= wait_ms; t += 250) {
        if (nucleo_anima_try_lock()) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return false;
}

// One owner message through ANIMA -> the text to send back (`out`).
void answer(const char *text, bool en, char *out, size_t cap, const char *image = nullptr)
{
    nucleo_anima_init(en ? "en" : "it");
    if (!lock_engine(15000)) {
        snprintf(out, cap, "%s", en ? "I'm busy on the device right now, try again in a moment." : "Sono occupata sul dispositivo, riprova tra un attimo.");
        return;
    }
    anima_result_t *r = (anima_result_t *)heap_caps_malloc(sizeof(anima_result_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r) { nucleo_anima_unlock(); snprintf(out, cap, "%s", en ? "Out of memory." : "Memoria esaurita."); return; }
    if (image && image[0]) nucleo_anima_attach_image(image);   // a photo sent with the message
    *r = nucleo_anima_query(text, en ? "en" : "it");
    const char *lr = nucleo_anima_long_reply();
    const char *reply = (lr && lr[0]) ? lr : r->reply;
    char note[160] = "";
    bool done = false;
    if (r->action == ANIMA_ACT_TOOL) done = nv_anima_os_run(r, en, note, sizeof note);   // under the gate (engine state)
    if (r->action == ANIMA_ACT_SYSTEM) nv_anima_system_reply(r->arg, reply, en, out, cap);
    else {
        snprintf(out, cap, "%s", reply[0] ? reply : (en ? "I don't know." : "Non lo so."));
        if (r->action == ANIMA_ACT_LAUNCH && r->arg[0]) nv_anima_pretty_launch(out, cap, r->arg);
    }
    nucleo_anima_unlock();
    if (r->action == ANIMA_ACT_LAUNCH && r->arg[0]) nv_ui_open_app_id_async(r->arg);   // on the device's screen
    if (r->action == ANIMA_ACT_TOOL && note[0]) {
        const size_t n = strlen(out);
        snprintf(out + n, cap - n, "\n%s %s", done ? "✓" : "✗", note);
    }
    heap_caps_free(r);
}

void channel_task(void *)
{
    NV_PSRAM_BSS static anima_tg_msg_t msg[4];   // ~6 KB: PSRAM, never the scarce internal RAM
    NV_PSRAM_BSS static char reply[4000];
    int64_t fast_until = 0;
    for (;;) {
        const bool en = nv_i18n_get_lang() != NV_LANG_IT;
        if (nucleo_anima_online_available()) nucleo_anima_tg_check_pending(en);   // a token pasted in the web Settings
        anima_tg_status_t st;
        nucleo_anima_tg_status(&st);
        if (!st.enabled || nucleo_anima_get_net_mode() == ANIMA_NET_OFF || !nucleo_anima_online_available()) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        const int n = nucleo_anima_tg_poll(msg, 4);
        for (int i = 0; i < n; i++) {
            if (nucleo_anima_tg_accept(&msg[i], en, reply, sizeof reply)) {
                char img[200] = "";
                if (msg[i].photo[0] && nucleo_anima_tg_fetch(msg[i].photo, img, sizeof img) != 0) img[0] = 0;
                const char *q = msg[i].text[0] ? msg[i].text
                              : en ? "What is in this picture?" : "Cosa c'e' in questa foto?";
                if (msg[i].photo[0] && !img[0])
                    snprintf(reply, sizeof reply, "%s", en ? "I could not download the photo, try again." : "Non riesco a scaricare la foto, riprova.");
                else
                    answer(q, en, reply, sizeof reply, img);
            }
            if (reply[0] && !nucleo_anima_tg_send(msg[i].chat, reply)) ESP_LOGW(TAG, "send failed");
        }
        const int64_t now = esp_timer_get_time() / 1000;
        if (n > 0) fast_until = now + 2 * 60 * 1000;
        vTaskDelay(pdMS_TO_TICKS(n < 0 ? 30000 : now < fast_until ? 3000 : 15000));
    }
}

// ANIMA's shell tool (nucleo_anima_set_shell): one command line through the Terminal's shell without its
// screen. Generous timeout: `store install` downloads an app.
int anima_sh_exec(const char *line, char *out, int cap)
{
    if (lvgl_port_lock(1000)) { sh_start(); lvgl_port_unlock(); }   // the shell task, once (LVGL-thread call)
    bool trunc = false;
    const int st = sh_exec_capture(line, out, (size_t)cap, 180000, &trunc);
    if (trunc) {
        const size_t n = strlen(out);
        if (n + 24 < (size_t)cap) snprintf(out + n, cap - n, "\n...(output truncated)");
    }
    if (st == -2) snprintf(out + strlen(out), cap - strlen(out), "\n(timed out)");
    return st;
}

}  // namespace

void nv_anima_channels_start(void)
{
    nucleo_anima_set_shell(anima_sh_exec);   // the model may now use the device shell (ACT sh ...)
    if (s_task) return;
    // The cascade + TLS want the same roomy stack as the ANIMA workers; PSRAM keeps it off internal RAM.
    if (xTaskCreateWithCaps(channel_task, "anima_tg", 24 * 1024, nullptr, 3, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        s_task = nullptr;
}
