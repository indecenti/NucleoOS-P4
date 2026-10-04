// ANIMA as an agent over a language model (LLM mode, a LAN Ollama faked by anima_fakenet): what the
// device answers itself, what the model is told about "now", how tools are offered, and what happens
// when a small model writes an action inside a sentence instead of running it.
// Regression for 2026-10-04: "che ore sono?" went to qwen3.5:9b, which answered it had no clock and
// suggested "`ACT open_app secondscreen`" to the user.
#include "check.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
extern "C" {
#include "nucleo_anima.h"
#include "nucleo_anima_conv.h"
#include "anima_fakenet.h"
}

static anima_result_t ask(const char *q, bool en = false)
{
    nucleo_anima_try_lock();
    anima_result_t r = nucleo_anima_query(q, en ? "en" : "it");
    nucleo_anima_unlock();
    return r;
}

static void teacher(const char *json)
{
    FILE *t = fopen("anima_sd/data/anima/teacher.json", "w");
    if (t) { fputs(json, t); fclose(t); }
}

static bool model_dialed() { return fakenet_chat_count() > 0; }

static std::string s_value_time = "Sono le 19:25", s_value_date = "Oggi e sabato 4 ottobre 2026";
static bool s_clock_set = true;
static bool test_values(const char *key, bool en, char *out, size_t cap)
{
    (void)en;
    if (!s_clock_set) { out[0] = 0; return false; }
    if (!strcmp(key, "time"))    { snprintf(out, cap, "%s", s_value_time.c_str()); return true; }
    if (!strcmp(key, "date"))    { snprintf(out, cap, "%s", s_value_date.c_str()); return true; }
    if (!strcmp(key, "version")) { snprintf(out, cap, "NucleoOS 9.9.9"); return true; }
    return false;
}

static std::vector<std::string> ran;

int main()
{
    if (system("rm -rf anima_sd && mkdir -p anima_sd/data/anima") != 0) return 1;
    CHECK(nucleo_anima_init("it") == ESP_OK);
    fakenet_online(1);
    nucleo_anima_set_shell([](const char *line, char *o, int cap) -> int { ran.push_back(line); snprintf(o, cap, "ok"); return 0; });
    teacher("{\"provider\":\"local\",\"base\":\"http://192.168.1.30:11434/v1\",\"model\":\"qwen3.5:9b\"}");
    nucleo_anima_set_net_mode(ANIMA_NET_LLM);

    // 1. The clock is the device's: any phrasing that is ONLY about it never reaches the model.
    {
        static const char *const it_time[] = { "che ore sono?", "che ora è?", "che ora sono?", "che ore sono",
            "mi dici l'ora?", "sai che ore sono?", "che ora è adesso", "ora esatta", nullptr };
        for (int i = 0; it_time[i]; i++) {
            fakenet_clear();
            fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Non ho un orologio.\"}}]}");
            anima_result_t r = ask(it_time[i]);
            const bool ok = !strcmp(r.intent, "time") && r.tier == ANIMA_TIER_COMMAND && !model_dialed() && !r.degraded;
            CHECK(ok);
            if (!ok) std::fprintf(stderr, "  [%s] -> intent=%s tier=%d model=%d\n", it_time[i], r.intent, r.tier, model_dialed());
        }
        static const struct { const char *q; const char *intent; bool en; } other[] = {
            { "che giorno è oggi?", "date", false }, { "che data è oggi", "date", false },
            { "in che anno siamo?", "year", false }, { "what time is it?", "time", true },
            { "what's the time", "time", true }, { "what day is it today", "date", true },
            { "what year is it", "year", true }, { nullptr, nullptr, false } };
        for (int i = 0; other[i].q; i++) {
            fakenet_clear();
            fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"?\"}}]}");
            anima_result_t r = ask(other[i].q, other[i].en);
            const bool ok = !strcmp(r.intent, other[i].intent) && !model_dialed();
            CHECK(ok);
            if (!ok) std::fprintf(stderr, "  [%s] -> intent=%s model=%d\n", other[i].q, r.intent, model_dialed());
        }
    }

    // 2. ...but a question that only MENTIONS time is the model's: another city, opening hours, history.
    {
        static const char *const model_q[] = { "che ore sono a Tokyo?", "a che ora apre il museo egizio?",
            "che ora è a New York adesso", "a che ora tramonta il sole oggi", "what time is it in London", nullptr };
        for (int i = 0; model_q[i]; i++) {
            fakenet_clear();
            fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Risposta del modello.\"}}]}");
            anima_result_t r = ask(model_q[i], !strncmp(model_q[i], "what", 4));
            const bool ok = model_dialed() && r.tier == ANIMA_TIER_REMOTE;
            CHECK(ok);
            if (!ok) std::fprintf(stderr, "  [%s] -> intent=%s tier=%d model=%d\n", model_q[i], r.intent, r.tier, model_dialed());
        }
    }

    // 3. The model is told "now" in every request (end of the system prompt), from the OS's values.
    nucleo_anima_set_value_resolver(test_values);
    {
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ecco una poesia.\"}}]}");
        ask("scrivimi una poesia breve sul mare");
        const char *post = fakenet_chat_post();
        CHECK(strstr(post, "AMBIENTE (adesso") != nullptr);
        CHECK(strstr(post, "Oggi e sabato 4 ottobre 2026, Sono le 19:25") != nullptr);
        CHECK(strstr(post, "NucleoOS 9.9.9 su ESP32-P4") != nullptr && !strstr(post, "NucleoOS NucleoOS"));
        CHECK(strstr(post, "prevalgono su riassunti e memoria") != nullptr);   // the env block is the authority
        CHECK(strstr(post, "date -d \\\"+10 days\\\" +%A") && strstr(post, "date -u (UTC"));   // exact date maths: the shell
        s_clock_set = false;                                    // a clock never set is said, not guessed
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ok.\"}}]}");
        ask("raccontami una barzelletta");
        CHECK(strstr(fakenet_chat_post(), "non conosci la data") != nullptr && !strstr(fakenet_chat_post(), "Sono le"));
        // ...and then the device does not pretend either
        anima_result_t r = ask("che ore sono?");
        nucleo_anima_resolve_reply(&r, false);
        CHECK(!strcmp(r.intent, "time") && strstr(r.reply, "orologio non") != nullptr);
        s_clock_set = true;
    }

    // 4. Web conversations follow the same rule, and their transcript stores the value, not "{value}".
    {
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Non ho un orologio.\"}}]}");
        char cid[NV_CONV_ID_CAP] = "";
        anima_result_t f;
        nucleo_anima_try_lock();
        int rc = nucleo_anima_conv_chat(nullptr, "che ore sono?", false, &f, cid, sizeof cid);
        nucleo_anima_unlock();
        CHECK(rc == 1 && !strcmp(f.intent, "time") && !model_dialed() && strstr(f.reply, "Sono le 19:25"));
        char *msgs = nullptr;
        CHECK(nucleo_anima_conv_msgs_json(cid, 10, &msgs) >= 0 && msgs && strstr(msgs, "Sono le 19:25") && !strstr(msgs, "{value}"));
        free(msgs);
        // a real question still goes to the model in the same conversation
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Il Nilo e' lungo circa 6650 km.\"}}]}");
        nucleo_anima_try_lock();
        rc = nucleo_anima_conv_chat(cid, "quanto e' lungo il Nilo e perche' e' importante?", false, &f, cid, sizeof cid);
        nucleo_anima_unlock();
        CHECK(rc == 1 && model_dialed() && strstr(f.reply, "6650"));
        CHECK(nucleo_anima_device_exact("apri la calcolatrice", false) && nucleo_anima_device_exact("quanto fa 12 per 12", false));
        CHECK(!nucleo_anima_device_exact("scrivi un programma in lua", false) && !nucleo_anima_device_exact("che ore sono a Parigi", false));
    }

    // 5. An action written inside a sentence is never run, never shown: the model is asked once to act.
    {
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"Non posso vederlo da qui, ma puoi chiedermelo con `ACT open_app secondscreen` per vederlo.\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh free -h\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Hai 300 KB liberi.\"}}]}");
        anima_result_t r = ask("verifica lo stato del sistema e dimmi se e' tutto a posto");
        CHECK(ran.size() == 1 && ran[0] == "free -h");
        CHECK(strstr(r.reply, "300 KB") && !strstr(r.reply, "ACT") && strstr(r.trace, "nudge"));
        CHECK(strcmp(r.intent, "open_app") != 0);
        CHECK(strstr(fakenet_chat_post(), "NON e' stata eseguita") != nullptr);
        // a model that keeps describing: after the nudges the text is cleaned, nothing launched
        ran.clear();
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"Per farlo usa `ACT open_app settings` e poi scegli Schermo.\"}}]}");
        r = ask("come cambio lo sfondo del mio telefono?");
        CHECK(ran.empty() && strcmp(r.intent, "open_app") != 0 && !strstr(r.reply, "ACT") && strstr(r.reply, "Schermo"));
        // a command SHOWN in a turn where nothing ran ("Calcolato con `date -d ...`", a ```bash block) is a
        // promise, not an answer: the model is asked to run it, and answers with the real output
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"Sara' giovedi'. Calcolato con `date -d \\\"+10 days\\\" +%A` sul dispositivo.\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh date -d \\\"+10 days\\\" +%A\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Sara' mercoledi'.\"}}]}");
        r = ask("che giorno della settimana sara' tra dieci giorni rispetto a oggi, piu' o meno?");
        CHECK(ran.size() == 1 && !strncmp(ran[0].c_str(), "date -d", 7) && strstr(r.reply, "mercoledi"));
        CHECK(strstr(fakenet_chat_post(), "non hai eseguito nessun comando") != nullptr);
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"```bash\nls -1 ~/ | wc -l\n```\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh ls -1 ~/ | wc -l\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ci sono 18 elementi.\"}}]}");
        r = ask("mi conti gli elementi nella mia cartella principale?");
        CHECK(ran.size() == 1 && strstr(r.reply, "18 elementi"));
        // ...but when the user ASKS for the command or a script, showing it is the answer
        ran.clear();
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Usa `ls -1 | wc -l`.\"}}]}");
        r = ask("qual e' il comando per contare i file di una cartella?");
        CHECK(ran.empty() && fakenet_chat_count() == 1 && strstr(r.reply, "ls -1"));
        // an ACT on its own line is a decision and still runs
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Lo apro.\\nACT open_app calc\"}}]}");
        r = ask("mi serve fare dei conti complicati con le tasse");
        CHECK(r.action == ANIMA_ACT_LAUNCH && !strcmp(r.arg, "calc"));
    }

    // 6. Tools are offered from what the server says, and asked again when it did not answer.
    {
        teacher("{\"provider\":\"local\",\"base\":\"http://192.168.1.31:11434/v1\",\"model\":\"qwen3.5:9b\"}");
        fakenet_clear();                                        // /api/show down: no tools, text grammar
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ok.\"}}]}");
        ask("spiegami in breve la teoria della relativita");
        CHECK(!strstr(fakenet_chat_post(), "\"tools\"") && strstr(fakenet_chat_post(), "ACT sh"));
        fakenet_clear();                                        // the server is up now, but within the window
        fakenet_add("/api/show", 200, "{\"capabilities\":[\"completion\",\"tools\"]}");
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ok.\"}}]}");
        ask("spiegami in breve la meccanica quantistica");
        CHECK(!strstr(fakenet_chat_post(), "\"tools\""));
        fakeclock_advance(61LL * 1000000);                      // after it: asked again, tools on
        ask("spiegami in breve la teoria dei giochi");
        CHECK(strstr(fakenet_chat_post(), "\"tools\"") && strstr(fakenet_chat_post(), "STRUMENTI"));
        CHECK(strstr(fakenet_chat_post(), "chiamale con lo strumento device"));   // ACT list = catalogue, not syntax
        CHECK(strstr(fakenet_chat_post(), "non scrivere mai un comando o una riga ACT"));
        // a server that says it has no tools is believed
        teacher("{\"provider\":\"local\",\"base\":\"http://192.168.1.32:11434/v1\",\"model\":\"qwen3.5:9b\"}");
        fakenet_clear();
        fakenet_add("/api/show", 200, "{\"capabilities\":[\"completion\"]}");
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ok.\"}}]}");
        ask("spiegami in breve la termodinamica");
        CHECK(!strstr(fakenet_chat_post(), "\"tools\""));
    }

    nucleo_anima_set_value_resolver(nullptr);
    nucleo_anima_set_shell(nullptr);
    return TEST_DONE("anima_agent");
}
