// The Telegram channel (OpenClaw's "channels"): talk to ANIMA from anywhere through a Telegram bot,
// and get the heartbeat's notifications there.
//
// Setup: create a bot with @BotFather, paste its token in Settings (web) > IA > Telegram, then send the
// bot "/pair <code>" with the 6-digit code shown on the device. Only that chat is answered afterwards;
// everyone else gets a short refusal. The token and the owner live in /data/anima/telegram.json, sealed
// to this chip like the API keys.
//
// No webhook (the device has no public address): the OS task polls getUpdates with timeout=0, quickly
// right after an exchange and slowly otherwise, so a TLS session is never held for long and ANIMA's own
// cloud calls never wait behind a parked long-poll.
#include "nucleo_anima.h"
#include "anima_internal.h"
#include "nucleo_board.h"
#include "nv_sealed.h"
#include "cJSON.h"
#include "nv_mem_attr.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#ifdef ANIMA_HOST
#define tg_random() ((uint32_t)rand())
#else
#include "esp_random.h"
#define tg_random() esp_random()
#endif

#define TG_PATH  NUCLEO_SD_MOUNT "/data/anima/telegram.json"
#define TG_API   "https://api.telegram.org/bot"

NV_PSRAM_BSS static struct {
    bool loaded, on;
    char token[96];
    char bot[48];
    long long owner;
    long long offset;          // next update_id to ask for
    char code[8];              // pairing code for this boot
    char err[96];              // last problem, for the UIs
    char pending[96];          // a token from the web, checked by the channel task (TLS off httpd)
    bool checking;
} s_tg;

static void tg_load(void)
{
    if (s_tg.loaded) return;
    s_tg.loaded = true;
    char *b = nv_sealed_read(TG_PATH, 4096, NULL);
    if (!b) return;
    cJSON *o = cJSON_Parse(b);
    free(b);
    if (!o) return;
    cJSON *t = cJSON_GetObjectItem(o, "token"), *w = cJSON_GetObjectItem(o, "owner"),
          *n = cJSON_GetObjectItem(o, "bot"), *on = cJSON_GetObjectItem(o, "on");
    if (cJSON_IsString(t)) snprintf(s_tg.token, sizeof s_tg.token, "%s", t->valuestring);
    if (cJSON_IsString(n)) snprintf(s_tg.bot, sizeof s_tg.bot, "%s", n->valuestring);
    if (cJSON_IsNumber(w)) s_tg.owner = (long long)w->valuedouble;
    s_tg.on = !cJSON_IsBool(on) || cJSON_IsTrue(on);
    cJSON_Delete(o);
}

static bool tg_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "token", s_tg.token);
    cJSON_AddStringToObject(o, "bot", s_tg.bot);
    cJSON_AddNumberToObject(o, "owner", (double)s_tg.owner);
    cJSON_AddBoolToObject(o, "on", s_tg.on);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    const bool ok = txt && nv_sealed_write(TG_PATH, txt, strlen(txt));
    cJSON_free(txt);
    return ok;
}

const char *nucleo_anima_tg_pair_code(void)
{
    if (!s_tg.code[0]) snprintf(s_tg.code, sizeof s_tg.code, "%06u", (unsigned)(tg_random() % 1000000u));
    return s_tg.code;
}

void nucleo_anima_tg_status(anima_tg_status_t *st)
{
    tg_load();
    memset(st, 0, sizeof *st);
    st->configured = s_tg.token[0] != 0;
    st->enabled = st->configured && s_tg.on;
    st->paired = s_tg.owner != 0;
    st->checking = s_tg.checking;
    snprintf(st->bot, sizeof st->bot, "%s", s_tg.bot);
    snprintf(st->code, sizeof st->code, "%s", nucleo_anima_tg_pair_code());
    snprintf(st->error, sizeof st->error, "%s", s_tg.err);
}

// A plausible bot token: digits ':' then 30+ of [A-Za-z0-9_-].
static bool token_ok(const char *t)
{
    const char *c = strchr(t, ':');
    if (!c || c == t || strlen(c + 1) < 30) return false;
    for (const char *p = t; p < c; p++) if (!isdigit((unsigned char)*p)) return false;
    for (const char *p = c + 1; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return false;
    return true;
}

int nucleo_anima_tg_set_token(const char *token, bool en)
{
    tg_load();
    if (!token || !token_ok(token)) {
        snprintf(s_tg.err, sizeof s_tg.err, "%s", en ? "that does not look like a bot token" : "non sembra un token di bot");
        return 0;
    }
    char url[200], *body = NULL;
    snprintf(url, sizeof url, TG_API "%s/getMe", token);
    if (anima_net_get(url, &body) <= 0) {
        snprintf(s_tg.err, sizeof s_tg.err, "%s", en ? "Telegram refused the token (or no internet)" : "Telegram rifiuta il token (o manca internet)");
        return 0;
    }
    cJSON *o = cJSON_Parse(body);
    free(body);
    cJSON *r = o ? cJSON_GetObjectItem(o, "result") : NULL, *u = r ? cJSON_GetObjectItem(r, "username") : NULL;
    const bool ok = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "ok")) && cJSON_IsString(u);
    if (ok) {
        if (strcmp(s_tg.token, token)) s_tg.owner = 0;          // a new bot: pair again
        snprintf(s_tg.token, sizeof s_tg.token, "%s", token);
        snprintf(s_tg.bot, sizeof s_tg.bot, "%s", u->valuestring);
        s_tg.on = true; s_tg.offset = 0; s_tg.err[0] = 0;
        tg_save();
    } else snprintf(s_tg.err, sizeof s_tg.err, "%s", en ? "unexpected answer from Telegram" : "risposta inattesa da Telegram");
    cJSON_Delete(o);
    return ok;
}

void nucleo_anima_tg_request_token(const char *token)
{
    tg_load();
    snprintf(s_tg.pending, sizeof s_tg.pending, "%s", token ? token : "");
    s_tg.checking = s_tg.pending[0] != 0;
    s_tg.err[0] = 0;
}

int nucleo_anima_tg_check_pending(bool en)
{
    if (!s_tg.pending[0]) return -1;
    char t[sizeof s_tg.pending];
    snprintf(t, sizeof t, "%s", s_tg.pending);
    s_tg.pending[0] = 0;
    const int ok = nucleo_anima_tg_set_token(t, en);
    s_tg.checking = false;
    return ok;
}

void nucleo_anima_tg_set_enabled(bool on) { tg_load(); s_tg.on = on; tg_save(); }
void nucleo_anima_tg_unlink(void) { tg_load(); s_tg.owner = 0; s_tg.code[0] = 0; tg_save(); }
void nucleo_anima_tg_forget(void) { tg_load(); memset(&s_tg, 0, sizeof s_tg); s_tg.loaded = true; tg_save(); }

int nucleo_anima_tg_poll(anima_tg_msg_t *m, int max)
{
    tg_load();
    if (!s_tg.token[0] || !s_tg.on || max <= 0) return 0;
    char url[240], *body = NULL;
    snprintf(url, sizeof url, TG_API "%s/getUpdates?timeout=0&limit=%d&allowed_updates=%%5B%%22message%%22%%5D&offset=%lld",
             s_tg.token, max, s_tg.offset);
    if (anima_net_get(url, &body) <= 0) return -1;
    cJSON *o = cJSON_Parse(body);
    free(body);
    cJSON *res = o ? cJSON_GetObjectItem(o, "result") : NULL;
    int n = 0;
    const int k = cJSON_IsArray(res) ? cJSON_GetArraySize(res) : 0;
    for (int i = 0; i < k; i++) {
        cJSON *u = cJSON_GetArrayItem(res, i);
        cJSON *id = cJSON_GetObjectItem(u, "update_id"), *msg = cJSON_GetObjectItem(u, "message");
        if (cJSON_IsNumber(id) && (long long)id->valuedouble >= s_tg.offset) s_tg.offset = (long long)id->valuedouble + 1;
        cJSON *chat = msg ? cJSON_GetObjectItem(msg, "chat") : NULL, *txt = msg ? cJSON_GetObjectItem(msg, "text") : NULL;
        cJSON *cid = chat ? cJSON_GetObjectItem(chat, "id") : NULL, *from = msg ? cJSON_GetObjectItem(msg, "from") : NULL;
        cJSON *fn = from ? cJSON_GetObjectItem(from, "first_name") : NULL;
        // A photo (the largest size under 1.9 MB) or an image sent as a file; the caption is the text.
        const char *photo = NULL;
        cJSON *ph = msg ? cJSON_GetObjectItem(msg, "photo") : NULL, *doc = msg ? cJSON_GetObjectItem(msg, "document") : NULL;
        if (cJSON_IsArray(ph)) {
            for (int j = cJSON_GetArraySize(ph) - 1; j >= 0 && !photo; j--) {
                cJSON *sz = cJSON_GetArrayItem(ph, j), *fid = cJSON_GetObjectItem(sz, "file_id"), *fs = cJSON_GetObjectItem(sz, "file_size");
                if (cJSON_IsString(fid) && (!cJSON_IsNumber(fs) || fs->valuedouble <= 1.9e6)) photo = fid->valuestring;
            }
        } else if (doc) {
            cJSON *mt = cJSON_GetObjectItem(doc, "mime_type"), *fid = cJSON_GetObjectItem(doc, "file_id"), *fs = cJSON_GetObjectItem(doc, "file_size");
            if (cJSON_IsString(mt) && (!strcmp(mt->valuestring, "image/jpeg") || !strcmp(mt->valuestring, "image/png")) &&
                cJSON_IsString(fid) && (!cJSON_IsNumber(fs) || fs->valuedouble <= 1.9e6)) photo = fid->valuestring;
        }
        if (!txt && photo) txt = cJSON_GetObjectItem(msg, "caption");
        if (!cJSON_IsNumber(cid) || (!cJSON_IsString(txt) && !photo) || n >= max) continue;   // stickers, voice...: skipped
        m[n].chat = (long long)cid->valuedouble;
        snprintf(m[n].from, sizeof m[n].from, "%s", cJSON_IsString(fn) ? fn->valuestring : "");
        snprintf(m[n].text, sizeof m[n].text, "%s", cJSON_IsString(txt) ? txt->valuestring : "");
        snprintf(m[n].photo, sizeof m[n].photo, "%s", photo ? photo : "");
        n++;
    }
    cJSON_Delete(o);
    return n;
}

int nucleo_anima_tg_fetch(const char *file_id, char *path, int cap)
{
    tg_load();
    if (!s_tg.token[0] || !file_id || !file_id[0]) return -1;
    char url[400], *body = NULL;
    snprintf(url, sizeof url, TG_API "%s/getFile?file_id=%.200s", s_tg.token, file_id);
    if (anima_net_get(url, &body) <= 0) return -1;
    cJSON *o = cJSON_Parse(body);
    free(body);
    cJSON *res = o ? cJSON_GetObjectItem(o, "result") : NULL, *fp = res ? cJSON_GetObjectItem(res, "file_path") : NULL;
    if (!cJSON_IsString(fp)) { cJSON_Delete(o); return -1; }
    snprintf(url, sizeof url, "https://api.telegram.org/file/bot%s/%.200s", s_tg.token, fp->valuestring);
    const char *ext = strrchr(fp->valuestring, '.');
    const bool png = ext && !strcasecmp(ext, ".png");
    cJSON_Delete(o);
    char *img = NULL;
    int st = 0;
    const int n = nucleo_anima_http_relay(url, "GET", NULL, NULL, 2 * 1024 * 1024, &img, &st);
    if (n <= 0 || st != 200) { free(img); return -1; }
    mkdir(NUCLEO_SD_MOUNT "/home", 0777);
    mkdir(NUCLEO_SD_MOUNT "/home/inbox", 0777);
    snprintf(path, cap, NUCLEO_SD_MOUNT "/home/inbox/tg-%lld.%s", (long long)time(NULL), png ? "png" : "jpg");
    FILE *f = fopen(path, "wb");
    const bool ok = f && fwrite(img, 1, (size_t)n, f) == (size_t)n;
    if (f) fclose(f);
    free(img);
    return ok ? 0 : -1;
}

int nucleo_anima_tg_accept(const anima_tg_msg_t *m, bool en, char *reply, int cap)
{
    tg_load();
    reply[0] = 0;
    if (s_tg.owner && m->chat == s_tg.owner) {
        if (!strcmp(m->text, "/start") || !strcmp(m->text, "/help")) {
            snprintf(reply, cap, "%s", en ? "I'm ANIMA. Ask me anything, or give me a command (\"turn the volume down\")."
                                          : "Sono ANIMA. Chiedimi qualcosa o dammi un comando (\"abbassa il volume\").");
            return 0;
        }
        return 1;                                         // the owner: run it through ANIMA
    }
    // Not the owner: only the pairing command is understood.
    if (!strncmp(m->text, "/pair ", 6) || !strncmp(m->text, "/start ", 7)) {
        const char *c = strchr(m->text, ' ');
        while (c && *c == ' ') c++;
        if (!s_tg.owner && c && !strcmp(c, nucleo_anima_tg_pair_code())) {
            s_tg.owner = m->chat;
            s_tg.code[0] = 0;                             // single use
            tg_save();
            snprintf(reply, cap, en ? "Paired. Hi %s, I'm ANIMA: ask me anything." : "Collegato. Ciao %s, sono ANIMA: chiedimi pure.", m->from);
            return 0;
        }
        snprintf(reply, cap, "%s", en ? "Wrong or used code. Read the one on the device (Settings > Anima)."
                                      : "Codice sbagliato o già usato. Leggi quello sul dispositivo (Impostazioni > Anima).");
        return 0;
    }
    snprintf(reply, cap, "%s", s_tg.owner ? (en ? "This ANIMA is private." : "Questo ANIMA è privato.")
                                          : (en ? "Send /pair followed by the code shown on the device." : "Invia /pair seguito dal codice mostrato sul dispositivo."));
    return 0;
}

bool nucleo_anima_tg_send(long long chat, const char *text)
{
    tg_load();
    if (!s_tg.token[0] || !chat || !text || !text[0]) return false;
    char url[200];
    snprintf(url, sizeof url, TG_API "%s/sendMessage", s_tg.token);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "chat_id", (double)chat);
    char clip[4000];                                       // Telegram's limit is 4096 characters
    snprintf(clip, sizeof clip, "%s", text);
    cJSON_AddStringToObject(o, "text", clip);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char *out = NULL;
    const int n = body ? anima_net_post_json(url, body, &out) : -1;
    cJSON_free(body);
    free(out);
    return n > 0;
}

bool nucleo_anima_tg_notify(const char *text)
{
    tg_load();
    return s_tg.on && s_tg.owner && nucleo_anima_tg_send(s_tg.owner, text);
}
