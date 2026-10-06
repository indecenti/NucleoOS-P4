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
#include "anima_internal.h"   // a_asks_to_run, a_asks_to_fix_file
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
        CHECK(strstr(r.trace, "nudge") != nullptr);             // the trace says it was asked to act
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
        // the same in a web conversation
        {
            ran.clear();
            fakenet_clear();
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Tra 10 giorni sara' sabato.\\n\\n```bash\\ndate -d \\\"+10 days\\\" +%A\\n```\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh date -d \\\"+10 days\\\" +%A\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Sara' mercoledi'.\"}}]}");
            char wcid[NV_CONV_ID_CAP] = "";
            anima_result_t wf;
            nucleo_anima_try_lock();
            nucleo_anima_conv_chat(nullptr, "che giorno della settimana sara' tra 10 giorni?", false, &wf, wcid, sizeof wcid);
            nucleo_anima_unlock();
            CHECK(ran.size() == 1 && strstr(wf.reply, "mercoledi"));
            if (ran.size() != 1) std::fprintf(stderr, "  web: ran=%zu reply=%s trace=%s chats=%d\n", ran.size(), wf.reply, wf.trace, fakenet_chat_count());
        }
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

        // "run it on the device": code with an invented "// Output" is not an answer (2026-10-05, js-run)
        FILE *pf = fopen("anima_sd/data/anima/permissions.json", "w");   // commands run without asking here
        if (pf) { fputs("{\"sh\":\"allow\"}", pf); fclose(pf); }
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"Il risultato e' `1-2-3`.\\n\\n```js\\nconsole.log([3,1,2].sort().join('-'))\\n// Output: 1-2-3\\n```\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"ACT sh js -e \\\"console.log([3,1,2].sort().join('-'))\\\"\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Stampa 1-2-3.\"}}]}");
        r = ask("Esegui con js sul dispositivo: console.log([3,1,2].sort().join('-')) e dimmi il risultato.");
        CHECK(ran.size() == 1 && !strncmp(ran[0].c_str(), "js -e", 5) && strstr(r.reply, "1-2-3"));
        CHECK(strstr(fakenet_chat_post(), "non hai eseguito nessun comando") != nullptr);
        // "correggilo": a fix shown in the reply leaves the file as it was -> asked once to save it
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh cat ~/t/bug.lua\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"Manca un end. Ecco il file corretto:\\n```lua\\nfor i=1,10 do s=s+i end\\nprint(s)\\n```\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh sed -i 's/s+i/s+i end/' ~/t/bug.lua\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh lua ~/t/bug.lua\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ora stampa 55.\"}}]}");
        r = ask("Il file ~/t/bug.lua ha un errore: correggilo, eseguilo e dimmi cosa stampa.");
        CHECK(ran.size() == 3 && !strncmp(ran[1].c_str(), "sed -i", 6) && strstr(r.reply, "55"));
        CHECK(strstr(r.trace, "nudge") != nullptr);
        // a fix that was written is not nudged
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh sed -i 's/a/b/' ~/t/x.lua\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Fatto, ho corretto il file.\"}}]}");
        r = ask("correggi il file ~/t/x.lua");
        CHECK(ran.size() == 1 && !strstr(r.trace, "nudge") && fakenet_chat_count() == 2);
        remove("anima_sd/data/anima/permissions.json");
    }

    // 5b. Found on the board with qwen3.5:9b (2026-10-05, tools/anima_eval.py --set hard).
    {
        FILE *pf = fopen("anima_sd/data/anima/permissions.json", "w");
        if (pf) { fputs("{\"sh\":\"allow\"}", pf); fclose(pf); }
        // a request about a FILE never runs the device command its words resemble: it set the volume to 100%
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh cat ~/e/config.json\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh sed -i 's/30/55/' ~/e/config.json\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Fatto: volume 55 nel file.\"}}]}");
        anima_result_t r = ask("Nel file ~/e/config.json cambia il valore di volume da 30 a 55 e conferma.");
        CHECK(strcmp(r.intent, "set_volume") != 0 && model_dialed() && ran.size() == 2 && !strstr(r.trace, "nudge"));
        CHECK(!nucleo_anima_device_exact("Nel file ~/e/config.json cambia il valore di volume da 30 a 55", false));
        CHECK(!nucleo_anima_device_exact("Il file utenti.json ha nome ed eta: quanti hanno piu' di 30 anni?", false));
        CHECK(!nucleo_anima_device_exact("calcola la media dei voti nel file ~/e/voti.csv", false));
        CHECK(nucleo_anima_device_exact("imposta il volume al 30%", false));          // the command itself still is
        CHECK(nucleo_anima_device_exact("quanto fa 30 per 55", false));
        // a compound the device can't run whole is the model's, on the web path too (was "(senza modello) Non ho capito")
        CHECK(!nucleo_anima_device_exact("Apri la calcolatrice, fai uno screenshot e dimmi cosa vedi sullo schermo.", false));
        CHECK(nucleo_anima_device_exact("imposta la luminosita al 40% e il volume al 20%", false));   // one device plan
        // a tool call the server left in the text (qwen XML) runs as one, and its markup is never shown
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"<tool_call>\\n<function=sh>\\n<parameter=command>\\ndatediff 2026-10-05 2027-01-01\\n</parameter>\\n</function>\\n</tool_call>\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Mancano 88 giorni.\\n</parameter>\\n</function>\\n</tool_call>\"}}]}");
        r = ask("quanti giorni mancano dal 2026-10-05 al 2027-01-01? usa gli strumenti del dispositivo");
        CHECK(ran.size() == 1 && ran[0] == "datediff 2026-10-05 2027-01-01");
        CHECK(strstr(r.reply, "88 giorni") && !strstr(r.reply, "</") && !strstr(r.reply, "tool_call"));
        // a chart of one number is cut (its value said in words); a real one fenced ```json is drawn as a chart
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"La derivata e' 3x^2 sin(x) + x^3 cos(x).\\n```chart\\n{\\\"type\\\":\\\"text\\\",\\\"series\\\":[{\\\"values\\\":[0]}]}\\n```\"}}]}");
        r = ask("spiegami cos'e' la derivata di x^3 sin x");
        CHECK(strstr(r.reply, "cos(x)") && !strstr(r.reply, "```") && !strstr(r.reply, "series"));
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"```json\\n{\\\"type\\\":\\\"bar\\\",\\\"title\\\":\\\"Giorni\\\",\\\"series\\\":[{\\\"values\\\":[88]}]}\\n```\"}}]}");
        r = ask("raccontami quanti giorni mancano a capodanno, piu' o meno");
        CHECK(strstr(r.reply, "Giorni: 88") && !strstr(r.reply, "```"));
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"Ecco:\\n```json\\n{\\\"type\\\":\\\"pie\\\",\\\"labels\\\":[\\\"A\\\",\\\"B\\\"],\\\"series\\\":[{\\\"values\\\":[3,5]}]}\\n```\"}}]}");
        r = ask("fammi un grafico a torta di A 3 e B 5");
        CHECK(strstr(r.reply, "```chart\n") && strstr(r.reply, "\"pie\""));
        remove("anima_sd/data/anima/permissions.json");
        // a write to "/home/..." (the home as WASI programs see it) with the body on the ACT line lands in ~/
        pf = fopen("anima_sd/data/anima/permissions.json", "w");
        if (pf) { fputs("{\"sh\":\"allow\",\"write\":\"allow\"}", pf); fclose(pf); }
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"ACT write /home/t/fizz.lua <<< \\\"print('FizzBuzz')\\\"\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Scritto.\"}}]}");
        system("mkdir -p anima_sd/home/t");
        r = ask("scrivi in lua un fizzbuzz nel file ~/t/fizz.lua");
        {
            char buf[64] = "";
            FILE *f = fopen("anima_sd/home/t/fizz.lua", "r");
            if (f) { size_t k = fread(buf, 1, sizeof buf - 1, f); buf[k] = 0; fclose(f); }
            CHECK(!strcmp(buf, "print('FizzBuzz')"));
            if (strcmp(buf, "print('FizzBuzz')")) std::fprintf(stderr, "  inline write: [%s] trace=%s reply=%s\n", buf, r.trace, r.reply);
        }
        // the same failing call over and over is stopped: told once, then the turn ends
        ran.clear();
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh cat ~/t/manca.txt\"}}]}");
        r = ask("leggimi il file ~/t/manca.txt e riassumilo");
        CHECK(ran.size() <= 4 && strstr(r.trace, "stuck") && fakenet_chat_count() <= 6);
        if (!strstr(r.trace, "stuck")) std::fprintf(stderr, "  stuck: ran=%zu chats=%d trace=%s intent=%s reply=%s\n", ran.size(), fakenet_chat_count(), r.trace, r.intent, r.reply);
        remove("anima_sd/data/anima/permissions.json");
        // ISO dates are not arithmetic (the calculator said "It's -10038")
        CHECK(!nucleo_anima_device_exact("Combien de jours y a-t-il entre le 2026-01-01 et le 2026-12-25 ?", true));
        CHECK(!nucleo_anima_device_exact("quanti giorni tra 2026-01-01 e 2026-12-25", false));
        // a web address is fetched, not remembered; a reply that only announces the next step gets it done
        pf = fopen("anima_sd/data/anima/permissions.json", "w");
        if (pf) { fputs("{\"sh\":\"allow\"}", pf); fclose(pf); }
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Il titolo e' <title>Example Domain</title>.\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh curl -s https://example.com | grep title\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Il comando e' fallito, provo cosi':\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh curl -s https://example.com | html2text | head -3\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Il titolo e' Example Domain.\"}}]}");
        r = ask("Scarica la pagina https://example.com e dimmi il suo titolo.");
        CHECK(ran.size() == 2 && strstr(r.reply, "Example Domain") && strstr(fakenet_chat_post(), "Hai annunciato il prossimo passo"));
        // a chart nobody asked for becomes its numbers in words
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":"
            "\"```chart\\n{\\\"type\\\":\\\"pie\\\",\\\"title\\\":\\\"File\\\",\\\"labels\\\":[\\\"lua\\\",\\\"altri\\\"],\\\"series\\\":[{\\\"values\\\":[2,5]}]}\\n```\"}}]}");
        r = ask("raccontami come sono divisi i miei file tra lua e altri, piu' o meno");
        CHECK(!strstr(r.reply, "```") && strstr(r.reply, "lua 2") && strstr(r.reply, "altri 5"));
        remove("anima_sd/data/anima/permissions.json");
        // a reply that claims the file was changed, with nothing run, is sent back; "Act sh" is still an action
        pf = fopen("anima_sd/data/anima/permissions.json", "w");
        if (pf) { fputs("{\"sh\":\"allow\"}", pf); fclose(pf); }
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ho aggiornato lo script ed eseguito: stampa 64.\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Act sh lua -e 'print(8*8)'\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Stampa 64.\"}}]}");
        r = ask("ora fallo arrivare fino a 8 e dimmi l'ultimo numero che stampa");
        CHECK(ran.size() == 1 && ran[0] == "lua -e 'print(8*8)'" && strstr(r.reply, "64") && !strstr(r.reply, "Act sh"));
        // ...but a poem "written" is an answer, not a claim
        ran.clear();
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ho scritto per te una poesia: il mare canta.\"}}]}");
        r = ask("scrivimi una poesia di una riga sul mare");
        CHECK(fakenet_chat_count() == 1 && !strstr(r.trace, "nudge"));
        remove("anima_sd/data/anima/permissions.json");
        // the model answers in the user's language
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ok.\"}}]}");
        ask("raccontami una storia breve");
        CHECK(strstr(fakenet_chat_post(), "nella lingua in cui e' scritto") != nullptr);
        // with no model at all the file request still never moves the device volume
        nucleo_anima_set_net_mode(ANIMA_NET_OFF);
        r = ask("Nel file ~/e/config.json cambia il valore di volume da 30 a 55 e conferma.");
        CHECK(strcmp(r.intent, "set_volume") != 0 && r.action != ANIMA_ACT_TOOL);
        if (!strcmp(r.intent, "set_volume") || r.action == ANIMA_ACT_TOOL) std::fprintf(stderr, "  offline file: intent=%s arg=%s\n", r.intent, r.arg);
        nucleo_anima_set_net_mode(ANIMA_NET_LLM);
    }

    // The request detectors behind those nudges.
    {
        CHECK(a_asks_to_run("Esegui con js sul dispositivo: console.log(1)"));
        CHECK(a_asks_to_run("Quanti giorni tra due date? Calcolalo con gli strumenti del dispositivo."));
        CHECK(a_asks_to_run("usa python sul dispositivo per calcolare 2**100"));
        CHECK(a_asks_to_run("run it and tell me the output"));
        CHECK(!a_asks_to_run("scrivimi una poesia sul mare"));
        CHECK(!a_asks_to_run("come funziona un dispositivo USB?"));
        CHECK(a_asks_to_fix_file("Il file ~/eval/bug.lua ha un errore: correggilo"));
        CHECK(a_asks_to_fix_file("fix main.py please"));
        CHECK(!a_asks_to_fix_file("correggi questa frase: io sono andato."));
        CHECK(!a_asks_to_fix_file("leggi il file ~/note.txt"));
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

    // 7. An Ollama server is driven natively: the device asks for the window it needs (Ollama's default
    //    4096 is nearly filled by the system prompt alone), and reads native replies and tool calls.
    {
        teacher("{\"provider\":\"local\",\"base\":\"http://192.168.1.40:11434/v1\",\"model\":\"qwen3.5:9b\"}");
        fakenet_clear();
        // the real answer is ~90 KB (license, template, modelfile): over the 32 KB body cap it was dropped,
        // so Ollama was never detected (no native tools, no native window)
        static std::string show = "{\"license\":\"" + std::string(90000, 'x') +
            "\",\"capabilities\":[\"completion\",\"tools\"],\"model_info\":{\"qwen35.context_length\":262144}}";
        fakenet_add("/api/show", 200, show.c_str());
        fakenet_add("/api/ps", 200, "{\"models\":[{\"name\":\"qwen3.5:9b\",\"context_length\":4096}]}");
        fakenet_add_once("/api/chat", 200, "{\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"function\":"
                         "{\"name\":\"sh\",\"arguments\":{\"command\":\"df -h\"}}}]},\"prompt_eval_count\":3100,\"eval_count\":12}");
        fakenet_add_once("/api/chat", 200, "{\"message\":{\"role\":\"assistant\",\"content\":\"Hai 17 GB liberi.\"},"
                         "\"prompt_eval_count\":3300,\"eval_count\":9}");
        ran.clear();
        anima_result_t r = ask("fai un controllo generale con il terminale e riassumimelo");
        const char *post = fakenet_chat_post();
        CHECK(strstr(fakenet_last_url(), "/api/") != nullptr);
        CHECK(fakenet_chat_count() == 2 && ran.size() == 1 && ran[0] == "df -h" && strstr(r.reply, "17 GB"));
        CHECK(strstr(post, "\"num_ctx\":16384") && strstr(post, "\"think\":false") && strstr(post, "\"stream\":false") &&
              strstr(post, "\"tools\"") && !strstr(post, "max_tokens") && !strstr(post, "reasoning_effort"));
        int used = 0, max = 0;
        nucleo_anima_ctx_stats(&used, &max);
        CHECK(max == 16384 && used == 3309);                    // the window asked for, Ollama's own count
        nucleo_anima_reset_session();                           // a new conversation: the meter starts over
        nucleo_anima_ctx_stats(&used, &max);
        CHECK(used == 0);
        // a small model gets its own maximum (never more than it has)
        teacher("{\"provider\":\"local\",\"base\":\"http://192.168.1.41:11434/v1\",\"model\":\"tinyllama\"}");
        fakenet_clear();
        fakenet_add("/api/show", 200, "{\"capabilities\":[\"completion\"],\"model_info\":{\"llama.context_length\":2048}}");
        fakenet_add("/api/chat", 200, "{\"message\":{\"role\":\"assistant\",\"content\":\"Ok.\"}}");
        ask("raccontami qualcosa di breve sui gatti");
        CHECK(strstr(fakenet_chat_post(), "\"num_ctx\":2048") != nullptr);
        // a server that is not Ollama (no /api/show) keeps the OpenAI endpoint
        teacher("{\"provider\":\"local\",\"base\":\"http://192.168.1.42:1234/v1\",\"model\":\"qwen3.5-9b\"}");
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ok.\"}}]}");
        ask("raccontami qualcosa di breve sui cani");
        CHECK(fakenet_chat_count() == 1 && strstr(fakenet_chat_post(), "max_tokens") && !strstr(fakenet_chat_post(), "num_ctx"));
    }

    // 7b. Run 4 on the board (2026-10-06): an announced step, an invented output, a re-run claimed after a write,
    //     an app opened mid-task, "add them up" (a sum, not an edit), a folder written as a file.
    {
        FILE *pf = fopen("anima_sd/data/anima/permissions.json", "w");
        if (pf) { fputs("{\"sh\":\"allow\",\"write\":\"allow\"}", pf); fclose(pf); }
        teacher("{\"provider\":\"local\",\"base\":\"http://192.168.1.44:11434/v1\",\"model\":\"qwen3.5:9b\"}");
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT write ~/t/v.csv\\n<<<\\nnome,voto\\nAnna,8\\nBruno,7\\n>>>\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"OK. Ora calcolo la media e te lo dico subito dopo.\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh awk -F, 'NR>1{s+=$2;n++}END{print s/n}' ~/t/v.csv\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"La media e' 7.5.\"}}]}");
        anima_result_t r = ask("crea ~/t/v.csv con Anna 8 e Bruno 7 e calcola la media dei voti");
        CHECK(ran.size() == 1 && strstr(r.reply, "7.5"));
        // a re-run claimed after a write, with nothing run since
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT write ~/t/c.py\\n<<<\\nprint(1)\\n>>>\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ho corretto il file e rieseguito i test: passano.\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh python ~/t/c.py\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Stampa 1: i test passano.\"}}]}");
        r = ask("scrivi ~/t/c.py con un test e dimmi se passa");
        CHECK(!ran.empty() && ran.back() == "python ~/t/c.py");               // (after the write's own syntax check)
        // an invented "Output:" with nothing run
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"```lua\\nfor i=1,3 do print(i*i) end\\n```\\n**Output:** 1 4 9\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh lua -e 'for i=1,3 do print(i*i) end'\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Stampa 1, 4, 9.\"}}]}");
        r = ask("ora fallo con i quadrati da 1 a 3 e dimmi cosa stampa");
        CHECK(ran.size() == 1 && strstr(r.reply, "9"));
        // an app opened mid-task: the loop goes on to the screenshot
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT open_app calc\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh screenshot\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Vedo la calcolatrice con i tasti.\"}}]}");
        r = ask("apri la calcolatrice, fai uno screenshot e dimmi cosa vedi sullo schermo");
        CHECK(ran.size() == 2 && ran[0] == "launch calc" && ran[1] == "screenshot" && strstr(r.reply, "calcolatrice"));
        if (ran.size() != 2) std::fprintf(stderr, "  open mid-task: ran=%zu trace=%s reply=%s intent=%s\n", ran.size(), r.trace, r.reply, r.intent);
        // a Lua App script "opened" as an app runs in Lua App; a reply that IS an invalid action is never shown raw
        system("mkdir -p anima_sd/home/lua && echo 'function nv.draw() end' > anima_sd/home/lua/contapassi.lua");
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT open_app contapassi\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"L'app contapassi e' pronta e gira senza errori.\"}}]}");
        r = ask("avvia la mia app lua contapassi e dimmi se ci sono errori");
        CHECK(ran.size() == 1 && ran[0] == "app run contapassi" && strstr(r.reply, "pronta"));
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT open_app nonesiste\"}}]}");
        r = ask("spiegami come si usa l'app nonesiste, se ce l'ho");
        CHECK(!strstr(r.reply, "ACT "));
        // a result that only a run can give, after a write and no run since
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT write ~/t/q.lua\\n<<<\\nfor i=1,8 do print(i*i) end\\n>>>\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"L'ultimo numero stampato e' 64.\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh lua ~/t/q.lua\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Stampa fino a 64.\"}}]}");
        r = ask("ora portalo fino a 8 nel file ~/t/q.lua e dimmi l'ultimo numero");
        CHECK(!ran.empty() && ran.back() == "lua ~/t/q.lua");
        // promises hidden before a code block, or a command shown in a plain fence, after a step
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh cat ~/t/x.py\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Il file non esiste. Scrivo il codice e poi lo eseguo:\\n```python\\nprint(1)\\n```\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh python -c 'print(1)'\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Stampa 1.\"}}]}");
        r = ask("scrivi ~/t/x.py che stampa 1 ed eseguilo");
        CHECK(ran.size() == 2 && ran[1] == "python -c 'print(1)'");
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh datediff 2026-10-05 2027-01-01 +%d\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Senza flag:\\n```\\ndatediff 2026-10-05 2027-01-01\\n```\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh datediff 2026-10-05 2027-01-01\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Sono 88 giorni.\"}}]}");
        r = ask("quanti giorni dal 2026-10-05 al 2027-01-01? usa gli strumenti del dispositivo");
        CHECK(ran.size() == 2 && strstr(r.reply, "88"));
        // "l'ultimo numero che stampa e' 64" next to code, with nothing run
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"```lua\\nfor n=1,8 do print(n*n) end\\n```\\nL'ultimo numero che stampa e' 64.\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh lua -e 'for n=1,8 do print(n*n) end'\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Stampa fino a 64.\"}}]}");
        r = ask("ora fallo arrivare fino a 8 e dimmi l'ultimo numero");
        CHECK(ran.size() == 1);
        // "add them up" is a sum, not an edit
        CHECK(!a_asks_to_fix_file("The file ~/t/n.txt has one number per line. Add them all up."));
        // a folder the system reads is never written as a file
        ran.clear();
        fakenet_clear();
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT write ~/lua\\n<<<\\nprint(1)\\n>>>\"}}]}");
        fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Ok.\"}}]}");
        ask("scrivi un'app lua che stampa 1 nella cartella delle app lua");
        FILE *lf = fopen("anima_sd/home/lua", "r");
        CHECK(lf == nullptr || !strstr(fakenet_chat_post(), "wrote"));
        if (lf) fclose(lf);
        CHECK(strstr(fakenet_chat_post(), "nome di una cartella") != nullptr);
        remove("anima_sd/data/anima/permissions.json");
    }

    // 8. A tool the schema never offered (qwen calls the program itself) runs as that command line; an empty
    //    200 answer fails the turn but never puts a healthy server on cooldown (2026-10-05: 7 turns muted).
    {
        FILE *pf = fopen("anima_sd/data/anima/permissions.json", "w");
        if (pf) { fputs("{\"sh\":\"allow\"}", pf); fclose(pf); }
        teacher("{\"provider\":\"local\",\"base\":\"http://192.168.1.43:11434/v1\",\"model\":\"qwen3.5:9b\"}");
        fakenet_clear();
        fakenet_add("/api/show", 200, "{\"capabilities\":[\"completion\",\"tools\"],\"model_info\":{\"qwen35.context_length\":32768}}");
        fakenet_add_once("/api/chat", 200, "{\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"function\":"
                         "{\"name\":\"datediff\",\"arguments\":{\"args\":\"2026-01-01 2026-12-25\"}}}]}}");
        fakenet_add_once("/api/chat", 200, "{\"message\":{\"role\":\"assistant\",\"content\":\"Sono 358 giorni.\"}}");
        ran.clear();
        anima_result_t r = ask("quanti giorni ci sono tra il primo gennaio e il 25 dicembre 2026? usa gli strumenti del dispositivo");
        CHECK(ran.size() == 1 && ran[0] == "datediff 2026-01-01 2026-12-25" && strstr(r.reply, "358"));
        // inline code under an interpreter's name goes to -c / -e
        fakenet_clear();
        fakenet_add("/api/show", 200, "{\"capabilities\":[\"completion\",\"tools\"],\"model_info\":{\"qwen35.context_length\":32768}}");
        fakenet_add_once("/api/chat", 200, "{\"message\":{\"role\":\"assistant\",\"content\":\"ACT python print([i*i for i in range(1,4)])\"}}");
        fakenet_add_once("/api/chat", 200, "{\"message\":{\"role\":\"assistant\",\"content\":\"Fa 1024.\"}}");
        ran.clear();
        r = ask("usa python sul dispositivo per stampare la lista dei quadrati da 1 a 3");
        CHECK(ran.size() == 1 && ran[0] == "python -c 'print([i*i for i in range(1,4)])'");
        if (ran.size() != 1 || ran[0] != "python -c 'print([i*i for i in range(1,4)])'") std::fprintf(stderr, "  inline py: %s\n", ran.empty() ? "-" : ran[0].c_str());
        // a 500 on a tool call (qwen's malformed XML) is retried once without the schemas; the server stays usable
        fakenet_clear();
        fakenet_add("/api/show", 200, "{\"capabilities\":[\"completion\",\"tools\"],\"model_info\":{\"qwen35.context_length\":32768}}");
        fakenet_add_once("/api/chat", 500, "{\"error\":\"XML syntax error on line 4: element <function> closed by </parameter>\"}");
        fakenet_add_once("/api/chat", 200, "{\"message\":{\"role\":\"assistant\",\"content\":\"Il mare e' blu per la luce diffusa.\"}}");
        r = ask("spiegami perche' il mare e' blu");
        CHECK(strstr(r.reply, "luce diffusa") && !strstr(fakenet_chat_post(), "\"tools\"") && nucleo_anima_model_usable());
        // an empty 200: this turn has no model answer, the next one dials the server again
        fakenet_clear();
        fakenet_add("/api/show", 200, "{\"capabilities\":[\"completion\",\"tools\"],\"model_info\":{\"qwen35.context_length\":32768}}");
        fakenet_add_once("/api/chat", 200, "{\"message\":{\"role\":\"assistant\",\"content\":\"\"}}");
        ask("raccontami una cosa curiosa sui polpi");
        CHECK(nucleo_anima_model_usable());
        remove("anima_sd/data/anima/permissions.json");
    }

    nucleo_anima_set_value_resolver(nullptr);
    nucleo_anima_set_shell(nullptr);
    return TEST_DONE("anima_agent");
}
