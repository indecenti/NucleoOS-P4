// ANIMA engine, end to end on the host: the real cascade (nucleo_anima_query) with the network
// stubbed offline and the SD at ./anima_sd. Checks the L0 commands, the tools, the solver and the
// session memory answer what they say - the device behaviour a user sees in the ANIMA app.
#include "check.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>
extern "C" {
#include "nucleo_anima.h"
#include "anima_fakenet.h"
}

static anima_result_t ask(const char *q, bool en = false)
{
    nucleo_anima_try_lock();
    anima_result_t r = nucleo_anima_query(q, en ? "en" : "it");
    nucleo_anima_unlock();
    return r;
}

// One turn: intent, arg (nullptr = don't care) and a reply fragment (nullptr = don't care).
static void expect(const char *q, const char *intent, const char *arg, const char *reply, bool en = false)
{
    anima_result_t r = ask(q, en);
    bool ok = !strcmp(r.intent, intent) && (!arg || !strcmp(r.arg, arg)) && (!reply || strstr(r.reply, reply));
    CHECK(ok);
    if (!ok) std::fprintf(stderr, "  [%s] -> intent=%s arg=%s reply=%s\n", q, r.intent, r.arg, r.reply);
}

int main()
{
    if (system("rm -rf anima_sd && mkdir -p anima_sd/data/anima") != 0) return 1;
    CHECK(nucleo_anima_init("it") == ESP_OK);

    // L0 commands
    expect("ciao", "greeting", nullptr, "Ciao");
    expect("chi sei?", "whoami", nullptr, "ANIMA");
    expect("grazie", "thanks", nullptr, nullptr);
    expect("che ore sono", "time", "time", nullptr);
    expect("what time is it", "time", "time", nullptr, true);

    // Apps: the named app wins over the "la/lo" follow-up pronoun
    expect("apri musica", "open_app", "music", nullptr);
    expect("apri la calcolatrice", "open_app", "calc", nullptr);
    expect("apri le note", "open_app", "notes", nullptr);
    expect("aprila", "open_app", "notes", nullptr);              // follow-up: the last app
    expect("chiudi la musica", "close_app", "music", nullptr);
    expect("open music", "open_app", "music", nullptr, true);

    // Device settings: absolute level vs a step
    expect("alza il volume", "set_volume", "+10", nullptr);
    expect("alza il volume di 20", "set_volume", "+20", "di 20");
    expect("abbassa la luminosità", "set_brightness", "-10", nullptr);
    expect("imposta la luminosità al 40", "set_brightness", "40", nullptr);

    // Solver
    expect("quanto fa 128 per 46", "calc", nullptr, "5888");
    expect("radice di 144", "calc", nullptr, "12");
    expect("fattoriale di 5", "calc", nullptr, "120");
    expect("5 km in metri", "convert", nullptr, "5000");
    expect("-10 c in f", "convert", nullptr, "14");            // a sign is kept
    expect("media di -5 e 5", "calc", nullptr, "0");
    expect("differenza tra 10 e -3", "calc", nullptr, "(-3) = 13");
    expect("che giorno era il 2024-05-01", "date", nullptr, "mercoled");
    expect("che giorno era il 31/02/2020", "date", nullptr, "1902");   // no such date: honest

    // Reminders keep the WHEN out of the text
    expect("ricordami domani alle 9 di chiamare Marco", "add_event", nullptr, "domani alle 09:00");
    CHECK(!strcmp(nucleo_anima_tool_content(), "off=1;time=09:00;text=chiamare Marco"));
    expect("ricordami tra 2 ore di bere", "add_event", nullptr, "bere");
    {   // a clock time two hours from now (maybe tomorrow), and the "tra 2 ore" words out of the text
        const char *c = nucleo_anima_tool_content();
        CHECK(strstr(c, ";time=") && !strstr(c, ";time=;") && strstr(c, "text=bere") && !strstr(c, "ore"));
        if (!strstr(c, "text=bere")) std::fprintf(stderr, "  content=%s\n", c);
    }
    expect("crea una nota con scritto comprare il latte", "create_file", "/data/Documents/nota.txt", nullptr);
    CHECK(strstr(nucleo_anima_tool_content(), "comprare il latte") != nullptr);
    expect("che impegni ho oggi", "agenda", "agenda:0:1", nullptr);
    expect("che impegni ho domani", "agenda", "agenda:1:1", nullptr);
    expect("cosa devo fare questa settimana", "agenda", "agenda:0:7", nullptr);
    CHECK(strcmp(ask("che impegni avevo ieri").intent, "agenda") != 0);   // the past: not the agenda

    // Profile memory
    expect("mi chiamo Niki", "profile", nullptr, "Niki");
    expect("come mi chiamo?", "profile", nullptr, "Niki");

    // Honest miss offline (no knowledge pack, no network): no fabricated answer
    anima_result_t r = ask("chi era Alan Turing?");
    CHECK(r.tier == ANIMA_TIER_NONE || r.confidence == 0);

    // Network modes: each one round-trips, and an offline-only device answers the same commands.
    for (int m : {ANIMA_NET_OFF, ANIMA_NET_LOCAL, ANIMA_NET_LLM, ANIMA_NET_HYBRID}) {
        nucleo_anima_set_net_mode(m);
        CHECK(nucleo_anima_get_net_mode() == m);
    }
    nucleo_anima_set_net_mode(ANIMA_NET_LOCAL);
    expect("quanto fa 6 per 7", "calc", nullptr, "42");
    nucleo_anima_set_net_mode(ANIMA_NET_HYBRID);

    // LLM tool-calling: a model's "ACT ..." line becomes a validated action; anything else does not.
    {
        anima_result_t a;
        CHECK(nucleo_anima_act_from_llm("ACT open_app calc", false, &a) && a.action == ANIMA_ACT_LAUNCH && !strcmp(a.arg, "calc"));
        CHECK(nucleo_anima_act_from_llm(" ACT set_volume 40%\n", false, &a) && a.action == ANIMA_ACT_TOOL && !strcmp(a.arg, "40"));
        // add_event / create_file leave something behind: by default the model must get a yes first
        CHECK(nucleo_anima_act_from_llm("ACT add_event 1 09:30 dentista", false, &a) && !strcmp(a.intent, "confirm") &&
              a.awaiting && strstr(a.reply, "procedo?"));
        anima_result_t y = ask("sì");
        CHECK(y.action == ANIMA_ACT_TOOL && !strcmp(y.intent, "add_event") &&
              !strcmp(nucleo_anima_tool_content(), "off=1;time=09:30;text=dentista"));
        CHECK(nucleo_anima_act_from_llm("ACT create_file spesa.txt | latte, pane", false, &a) && !strcmp(a.intent, "confirm"));
        y = ask("ok");
        CHECK(y.action == ANIMA_ACT_TOOL && !strcmp(y.arg, "/data/Documents/spesa.txt") && !strcmp(nucleo_anima_tool_content(), "latte, pane"));
        CHECK(nucleo_anima_act_from_llm("ACT create_file x.txt | y", false, &a) && !strcmp(a.intent, "confirm"));
        y = ask("no");
        CHECK(y.action == ANIMA_ACT_ANSWER && strstr(y.reply, "lascio stare"));
        CHECK(strcmp(ask("sì").intent, "create_file") != 0);                    // nothing pending any more
        // permissions.json: deny a tool, allow another without asking
        system("mkdir -p anima_sd/data/anima");
        FILE *pf = fopen("anima_sd/data/anima/permissions.json", "w");
        fputs("{\"set_volume\":\"deny\",\"create_file\":\"allow\"}", pf); fclose(pf);
        CHECK(nucleo_anima_act_from_llm("ACT set_volume 10", false, &a) && !strcmp(a.intent, "denied") && a.action == ANIMA_ACT_ANSWER);
        CHECK(nucleo_anima_act_from_llm("ACT create_file a.txt | b", false, &a) && a.action == ANIMA_ACT_TOOL);
        CHECK(nucleo_anima_permission("add_event") == 1 && nucleo_anima_permission("open_app") == 0);
        // A long rule set (pretty-printed past the old 600-byte read) still parses: the deny holds.
        pf = fopen("anima_sd/data/anima/permissions.json", "w");
        fputs("{\n", pf);
        for (int i = 0; i < 60; i++) fprintf(pf, "  \"tool_%02d\": \"ask\",\n", i);
        fputs("  \"open_app\": \"deny\"\n}", pf); fclose(pf);
        CHECK(nucleo_anima_permission("open_app") == 2);
        CHECK(nucleo_anima_set_agent_mode(1) && nucleo_anima_permission("open_app") == 2);   // rules kept
        nucleo_anima_set_agent_mode(0);
        // A broken file fails closed (ask), and set_agent_mode refuses to overwrite it.
        pf = fopen("anima_sd/data/anima/permissions.json", "w");
        fputs("{\"open_app\": \"deny\", oops", pf); fclose(pf);
        CHECK(nucleo_anima_permission("open_app") == 1);
        CHECK(!nucleo_anima_set_agent_mode(1));
        remove("anima_sd/data/anima/permissions.json");
        // The model may not rewrite its own permissions, persona or credential vaults.
        {
            char res[256];
            CHECK(nucleo_anima_file_tool("ACT write /sdcard/data/anima/permissions.json\n<<<\n{}\n>>>", true, res, sizeof res) == 1
                  && strstr(res, "not allowed"));
            CHECK(nucleo_anima_file_tool("ACT write /sdcard/data/anima/soul.md\n<<<\nx\n>>>", true, res, sizeof res) == 1
                  && strstr(res, "not allowed"));
            CHECK(nucleo_anima_file_tool("ACT write /sdcard/data/anima/notes.txt\n<<<\nx\n>>>", true, res, sizeof res) == 1
                  && strstr(res, "wrote"));
            remove("anima_sd/data/anima/notes.txt");
            // edit refuses a file over the 32 KB cap instead of truncating it on rewrite
            FILE *big = fopen("anima_sd/home/big.txt", "w");
            if (!big) { system("mkdir -p anima_sd/home"); big = fopen("anima_sd/home/big.txt", "w"); }
            for (int i = 0; i < 40 * 1024; i++) fputc(i == 10 ? 'Q' : 'x', big);
            fclose(big);
            CHECK(nucleo_anima_file_tool("ACT edit ~/big.txt\n<<<\nQ\n===\nR\n>>>", true, res, sizeof res) == 1
                  && strstr(res, "too large"));
            struct stat bst; CHECK(stat("anima_sd/home/big.txt", &bst) == 0 && bst.st_size == 40 * 1024);
            remove("anima_sd/home/big.txt");
        }
        // Local-host check: userinfo or a longer hostname cannot pass a public host as LAN.
        CHECK(nucleo_anima_url_is_local("http://10.0.0.5:8080/v1"));
        CHECK(nucleo_anima_url_is_local("http://192.168.1.20/api"));
        CHECK(nucleo_anima_url_is_local("http://pc.local:11434/v1"));
        CHECK(!nucleo_anima_url_is_local("http://10.0.0.1.evil.com/v1"));
        CHECK(!nucleo_anima_url_is_local("http://10.0.0.1@evil.com/v1"));
        CHECK(!nucleo_anima_url_is_local("http://evil.com/?h=10.0.0.1"));
        CHECK(!nucleo_anima_url_is_local("http://10.0.0.999/"));
        CHECK(!nucleo_anima_url_is_local("https://api.openai.com/v1"));
        CHECK(!nucleo_anima_act_from_llm("ACT open_app rm-rf", false, &a));
        CHECK(!nucleo_anima_act_from_llm("ACT set_volume 300", false, &a));
        CHECK(!nucleo_anima_act_from_llm("ACT create_file ../boot.bin | x", false, &a));
        CHECK(!nucleo_anima_act_from_llm("ACT format_sd", false, &a));
        CHECK(!nucleo_anima_act_from_llm("Ecco: ACT open_app calc", false, &a));
        CHECK(strstr(nucleo_anima_act_grammar(false), "ACT open_app") != nullptr);
        // ACT remember -> MEMORY.md (dated line), and it reaches the next prompt
        remove("anima_sd/data/anima/MEMORY.md");
        CHECK(nucleo_anima_act_from_llm("ACT remember Il gatto si chiama Pixel", false, &a) && !strcmp(a.intent, "remember") &&
              a.action == ANIMA_ACT_ANSWER && strstr(a.reply, "Pixel"));
        CHECK(!nucleo_anima_act_from_llm("ACT remember x", false, &a));                        // too short
        {
            FILE *mf = fopen("anima_sd/data/anima/MEMORY.md", "r"); char mb[256] = ""; size_t mn = mf ? fread(mb, 1, sizeof mb - 1, mf) : 0;
            if (mf) fclose(mf); mb[mn] = 0;
            CHECK(strstr(mb, "# MEMORY.md") && strstr(mb, "- Il gatto si chiama Pixel"));
            char wp[2600];
            CHECK(nucleo_anima_workspace_prompt(false, wp, sizeof wp) > 0 && strstr(wp, "COSA RICORDI") && strstr(wp, "Pixel"));
            std::string big; for (int i = 0; i < 200; i++) big += "riga di memoria numero " + std::to_string(i) + " abbastanza lunga\n";
            mf = fopen("anima_sd/data/anima/MEMORY.md", "a"); fputs(big.c_str(), mf); fclose(mf);
            CHECK(nucleo_anima_workspace_prompt(false, wp, sizeof wp) > 0 && strstr(wp, "numero 199") && !strstr(wp, "Pixel"));   // the most recent part
        }
        remove("anima_sd/data/anima/MEMORY.md");
    }

    // Skills on the SD: trigger match feeds the model's prompt; the offline line answers without network.
    {
        system("mkdir -p anima_sd/data/anima/skills");
        FILE *f = fopen("anima_sd/data/anima/skills/cucina.md", "w");
        fputs("---\nname: cucina\ndescription: ricette\ntriggers: ricetta, cosa cucino\n"
              "offline: Pasta al pomodoro in 15 minuti.\n---\nProponi UN piatto con dosi.\n", f);
        fclose(f);
        f = fopen("anima_sd/data/anima/skills/rotta.md", "w");
        fputs("niente intestazione\n", f);
        fclose(f);
        char buf[512], one[128];
        CHECK(nucleo_anima_skills_list(buf, sizeof buf) == 1 && !strcmp(buf, "cucina"));
        CHECK(nucleo_anima_skills_prompt("Cosa cucino stasera?", false, buf, sizeof buf) > 0 &&
              strstr(buf, "cucina") && strstr(buf, "UN piatto"));
        CHECK(nucleo_anima_skills_prompt("che ore sono", false, buf, sizeof buf) == 0);
        CHECK(nucleo_anima_skills_offline("mi dai una ricetta?", one, sizeof one) && strstr(one, "Pasta"));
        CHECK(!nucleo_anima_skills_offline("prericetta", one, sizeof one));   // word-bounded
        nucleo_anima_set_net_mode(ANIMA_NET_OFF);
        anima_result_t sr = ask("mi suggerisci una ricetta?");
        CHECK(!strcmp(sr.intent, "skill") && strstr(sr.reply, "Pasta"));
        nucleo_anima_set_net_mode(ANIMA_NET_HYBRID);
    }

    // ONLINE, end to end over the fake network: the live tools parse what the real services return,
    // and a model's ACT line becomes a real action.
    {
        fakenet_online(1);
        fakenet_add("geocoding-api.open-meteo.com", 200, "{\"results\":[{\"name\":\"Roma\",\"latitude\":41.89,\"longitude\":12.48}]}");
        fakenet_add("daily=sunrise", 200, "{\"daily\":{\"sunrise\":[\"2026-10-01T07:05\",\"2026-10-02T07:06\"],"
                                          "\"sunset\":[\"2026-10-01T18:52\",\"2026-10-02T18:50\"]}}");
        fakenet_add("api.open-meteo.com/v1/forecast", 200, "{\"current\":{\"temperature_2m\":21.4},\"daily\":{\"weather_code\":[1],"
                                          "\"temperature_2m_max\":[24.2],\"temperature_2m_min\":[15.1]}}");
        fakenet_add("news.google.com/rss/search", 200,
            "<rss><channel><title>Google News</title><item><title><![CDATA[Juve, vittoria &amp; primato - ANSA]]></title></item>"
            "<item><title>Seconda notizia - Sky</title></item></channel></rss>");
        // a front page longer than the fetch cap: the prefix still yields the first headlines
        static std::string big = "<rss><channel>";
        for (int i = 0; i < 400; i++) big += "<item><title>Titolo " + std::to_string(i) + " - Fonte</title><description>xxxxxxxxxxxxxxxxxxxxxxxx</description></item>";
        fakenet_add("news.google.com/rss?", 200, big.c_str());
        fakenet_add("api.coingecko.com", 200, "{\"bitcoin\":{\"eur\":52340.5,\"usd\":56100.2,\"eur_24h_change\":1.234}}");
        fakenet_add("date.nager.at", 200, "[{\"date\":\"2026-11-01\",\"localName\":\"Ognissanti\",\"name\":\"All Saints\"},"
                                          "{\"date\":\"2026-12-08\",\"localName\":\"Immacolata Concezione\",\"name\":\"Immaculate Conception\"}]");
        fakenet_add("api.frankfurter.app", 200, "{\"date\":\"2026-09-30\",\"rates\":{\"USD\":1.0912}}");

        anima_result_t w = ask("che tempo fa a Roma");
        CHECK(!strcmp(w.intent, "weather") && strstr(w.reply, "Roma") && strstr(w.reply, "21"));
        expect("a che ora tramonta il sole a Roma", "sun", nullptr, "18:52");
        expect("alba a roma domani", "sun", nullptr, "07:06");
        expect("ultime notizie su juventus", "news", nullptr, "Juve, vittoria & primato");
        CHECK(strstr(fakenet_last_url(), "q=juventus") != nullptr);
        expect("dammi le notizie di oggi", "news", nullptr, "Titolo 0");
        expect("quanto vale un bitcoin?", "crypto", nullptr, "52.340 €");
        expect("quali sono i prossimi giorni festivi", "holidays", nullptr, "1 novembre Ognissanti");
        expect("quanto vale un euro in dollari", "fx", nullptr, "1,0912");

        // a model's ACT line through the LLM tier (a keyless LAN server, Ollama-style)
        system("mkdir -p anima_sd/data/anima");
        FILE *t = fopen("anima_sd/data/anima/teacher.json", "w");
        fputs("{\"provider\":\"local\",\"base\":\"http://192.168.1.10:11434/v1\",\"model\":\"qwen2.5\"}", t);
        fclose(t);
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"ACT set_volume 30\"}}]}");
        nucleo_anima_set_net_mode(ANIMA_NET_LLM);
        anima_result_t a = ask("rendi il suono del dispositivo meno invadente");
        CHECK(a.action == ANIMA_ACT_TOOL && !strcmp(a.intent, "set_volume") && !strcmp(a.arg, "30"));
        CHECK(strstr(fakenet_last_post(), "ACT open_app") != nullptr);     // the grammar reached the model
        // the workspace files reach the model too
        t = fopen("anima_sd/data/anima/SOUL.md", "w"); fputs("Parla come un maggiordomo inglese.", t); fclose(t);
        t = fopen("anima_sd/data/anima/USER.md", "w"); fputs("Si chiama Niki, ha un gatto.", t); fclose(t);
        ask("rendi il suono del dispositivo meno invadente");
        CHECK(strstr(fakenet_last_post(), "maggiordomo") && strstr(fakenet_last_post(), "un gatto"));
        // heartbeat: no checklist -> nothing; HEARTBEAT_OK -> silent; anything else -> one notification
        char hb[256];
        CHECK(nucleo_anima_heartbeat("ore 10:00", false, hb, sizeof hb) == 0);
        t = fopen("anima_sd/data/anima/HEARTBEAT.md", "w"); fputs("- Impegni nelle prossime 2 ore?\n", t); fclose(t);
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"HEARTBEAT_OK\"}}]}");
        CHECK(nucleo_anima_heartbeat("ore 10:00, oggi: niente", false, hb, sizeof hb) == 0);
        CHECK(strstr(fakenet_last_post(), "Impegni nelle prossime 2 ore") && strstr(fakenet_last_post(), "ore 10:00"));
        fakenet_clear();
        fakenet_add("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Alle 11:00 hai il dentista.\"}}]}");
        CHECK(nucleo_anima_heartbeat("ore 10:00, oggi: 11:00 dentista", false, hb, sizeof hb) == 1 && strstr(hb, "dentista"));
        nucleo_anima_set_net_mode(ANIMA_NET_OFF);
        CHECK(nucleo_anima_heartbeat("x", false, hb, sizeof hb) == 0);           // offline: never a model call
        nucleo_anima_set_net_mode(ANIMA_NET_LLM);
        remove("anima_sd/data/anima/SOUL.md"); remove("anima_sd/data/anima/USER.md"); remove("anima_sd/data/anima/HEARTBEAT.md");
        nucleo_anima_set_net_mode(ANIMA_NET_HYBRID);

        // Speech-to-text: the home Whisper server first (no key), the cloud only when it fails.
        FILE *wav = fopen("anima_sd/v.wav", "wb"); fputs("RIFFfakeaudio", wav); fclose(wav);
        t = fopen("anima_sd/data/anima/teacher.json", "w");
        fputs("{\"provider\":\"groq\",\"key\":\"gsk_test\",\"stt_url\":\"http://192.168.1.20:8080/inference\"}", t);
        fclose(t);
        char txt[128], lg[8], where[64];
        CHECK(nucleo_anima_stt_route(where, sizeof where) == 1 && !strcmp(where, "192.168.1.20:8080"));
        fakenet_add("192.168.1.20:8080/inference", 200, "{\"text\":\" Ciao, come stai?\"}");
        CHECK(nucleo_anima_transcribe("anima_sd/v.wav", "auto", txt, sizeof txt, lg, sizeof lg) > 0 && strstr(txt, "Ciao"));
        CHECK(strstr(fakenet_last_url(), "192.168.1.20") && strstr(fakenet_last_post(), "RIFFfakeaudio") &&
              strstr(fakenet_last_post(), "name=\"file\""));
        fakenet_clear();                                      // home server down -> the cloud key answers
        fakenet_add("api.groq.com/openai/v1/audio/transcriptions", 200, "{\"text\":\"Hello there\",\"language\":\"english\"}");
        CHECK(nucleo_anima_transcribe("anima_sd/v.wav", "auto", txt, sizeof txt, lg, sizeof lg) > 0 &&
              !strcmp(txt, "Hello there") && !strcmp(lg, "en"));
        t = fopen("anima_sd/data/anima/teacher.json", "w"); fputs("{\"provider\":\"groq\",\"key\":\"gsk_test\"}", t); fclose(t);
        CHECK(nucleo_anima_stt_route(where, sizeof where) == 2 && !strcmp(where, "Groq"));
        nucleo_anima_set_net_mode(ANIMA_NET_LOCAL);           // LAN-only: never the cloud
        CHECK(nucleo_anima_stt_route(where, sizeof where) == 0);
        CHECK(nucleo_anima_transcribe("anima_sd/v.wav", "auto", txt, sizeof txt, lg, sizeof lg) < 0);
        // Agent loop over the device shell: the model runs a command, reads the output, answers.
        {
            nucleo_anima_set_net_mode(ANIMA_NET_LLM);
            static std::vector<std::string> ran;
            nucleo_anima_set_shell([](const char *line, char *o, int cap) -> int {
                ran.push_back(line);
                snprintf(o, cap, "%s", !strncmp(line, "df", 2) ? "/sdcard  29G  12G  17G  41%" : "Documents\nMusic\n");
                return 0;
            });
            CHECK(nucleo_anima_sh_class("ls -la /sdcard | grep mp3") == 1);
            CHECK(nucleo_anima_sh_class("rm -rf /sdcard/x") == 0 && nucleo_anima_sh_class("ls > out.txt") == 0);
            CHECK(nucleo_anima_sh_class("store search scacchi") == 1 && nucleo_anima_sh_class("store install chess") == 0);
            CHECK(nucleo_anima_sh_class("sed -i s/a/b/ f") == 0 && nucleo_anima_sh_class("edit notes.txt") == -1);
            fakenet_clear();
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh df -h\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh ls /sdcard\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Hai 17 GB liberi su 29.\"}}]}");
            anima_result_t sr = ask("quanto spazio libero mi resta sulla scheda?");
            CHECK(ran.size() == 2 && ran[0] == "df -h" && ran[1] == "ls /sdcard");
            CHECK(strstr(sr.reply, "17 GB") && strstr(sr.trace, "sh df -h") && strstr(sr.trace, "sh ls /sdcard"));
            CHECK(strstr(fakenet_last_post(), "OUTPUT of `ls /sdcard`") && strstr(fakenet_last_post(), "Documents"));
            // a command that changes something asks first (default), then runs on "sì"
            ran.clear(); fakenet_clear();
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh mkdir /sdcard/progetti\"}}]}");
            sr = ask("crea una cartella progetti sulla scheda");
            CHECK(ran.empty() && !strcmp(sr.intent, "confirm"));
            sr = ask("sì");
            CHECK(ran.size() == 1 && ran[0] == "mkdir /sdcard/progetti" && strstr(sr.reply, "$ mkdir"));
            // autonomous mode: the same runs at once; an explicit deny still holds
            FILE *pf = fopen("anima_sd/data/anima/permissions.json", "w"); fputs("{\"mode\":\"auto\"}", pf); fclose(pf);
            ran.clear(); fakenet_clear();
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh mkdir /sdcard/a\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Fatto.\"}}]}");
            sr = ask("crea la cartella a");
            CHECK(ran.size() == 1 && strstr(sr.reply, "Fatto"));
            pf = fopen("anima_sd/data/anima/permissions.json", "w"); fputs("{\"mode\":\"auto\",\"sh\":\"deny\"}", pf); fclose(pf);
            ran.clear(); fakenet_clear();
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT sh rm -rf /sdcard/a\"}}]}");
            sr = ask("cancella la cartella a");
            CHECK(ran.empty() && !strcmp(sr.intent, "denied"));
            // file tools in the loop (auto mode: no confirmation): write, read back through the shell, edit
            ran.clear(); fakenet_clear();
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT write ~/lua/ciao.lua\\n<<<\\nprint('ciao')\\nprint('mondo')\\n>>>\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT edit ~/lua/ciao.lua\\n<<<\\nprint('mondo')\\n===\\nprint('NucleoOS')\\n>>>\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Fatto: ho creato ciao.lua.\"}}]}");
            pf = fopen("anima_sd/data/anima/permissions.json", "w"); fputs("{\"mode\":\"auto\"}", pf); fclose(pf);
            sr = ask("scrivi uno script lua che saluta");
            {
                FILE *lf = fopen("anima_sd/home/lua/ciao.lua", "r"); char lb[128] = ""; size_t ln = lf ? fread(lb, 1, sizeof lb - 1, lf) : 0;
                if (lf) fclose(lf); lb[ln] = 0;
                CHECK(!strcmp(lb, "print('ciao')\nprint('NucleoOS')"));
            }
            CHECK(strstr(sr.reply, "ciao.lua") && strstr(sr.trace, "write") && strstr(sr.trace, "edit"));
            CHECK(strstr(fakenet_last_post(), "RESULT: edited /sdcard/home/lua/ciao.lua"));
            char fr[300];
            CHECK(nucleo_anima_file_tool("ACT write /etc/passwd\n<<<\nx\n>>>", false, fr, sizeof fr) && strstr(fr, "non consentito"));
            CHECK(nucleo_anima_file_tool("ACT write ~/../boot\n<<<\nx\n>>>", false, fr, sizeof fr) && strstr(fr, "non consentito"));
            nucleo_anima_file_tool("ACT write ~/t.txt\n<<<\na a\n>>>", false, fr, sizeof fr);
            CHECK(nucleo_anima_file_tool("ACT edit ~/t.txt\n<<<\na\n===\nb\n>>>", false, fr, sizeof fr) && strstr(fr, "more than once"));
            CHECK(nucleo_anima_file_tool("ACT edit ~/t.txt\n<<<\nzz\n===\nb\n>>>", false, fr, sizeof fr) && strstr(fr, "not in"));
            // without auto mode a write asks first, and "sì" writes the whole block
            remove("anima_sd/data/anima/permissions.json");
            anima_result_t wa;
            CHECK(nucleo_anima_act_from_llm("ACT write ~/n.txt\n<<<\nriga uno\nriga due\n>>>", false, &wa) && !strcmp(wa.intent, "confirm"));
            sr = ask("sì");
            {
                FILE *nf = fopen("anima_sd/home/n.txt", "r"); char nb[64] = ""; size_t nn = nf ? fread(nb, 1, sizeof nb - 1, nf) : 0;
                if (nf) fclose(nf); nb[nn] = 0;
                CHECK(!strcmp(nb, "riga uno\nriga due") && strstr(sr.reply, "wrote"));
            }
            pf = fopen("anima_sd/data/anima/permissions.json", "w"); fputs("{\"mode\":\"auto\",\"sh\":\"deny\"}", pf); fclose(pf);
            CHECK(nucleo_anima_auto_mode());
            CHECK(nucleo_anima_set_auto_mode(false) && !nucleo_anima_auto_mode() && nucleo_anima_permission("sh") == 2);   // deny kept
            // plan mode: read-only, the grammar asks for a plan; what changes something is denied
            CHECK(nucleo_anima_set_agent_mode(2) && nucleo_anima_agent_mode() == 2 && !nucleo_anima_auto_mode());
            CHECK(nucleo_anima_permission("write") == 2 && nucleo_anima_permission("add_event") == 2);
            CHECK(strstr(nucleo_anima_sh_grammar(true), "PLAN MODE") && !strstr(nucleo_anima_sh_grammar(true), "TODO:"));
            CHECK(nucleo_anima_set_agent_mode(0) && nucleo_anima_agent_mode() == 0 && nucleo_anima_permission("sh") == 2);
            CHECK(strstr(nucleo_anima_sh_grammar(false), "TODO:"));
            // multimodal: the model's capabilities, ACT see with a vision model, the vision helper
            CHECK(nucleo_anima_sh_class("screenshot") == 1 && nucleo_anima_sh_class("screenshot -d 3") == 1);
            CHECK(nucleo_anima_sh_class("screenshot /sdcard/data/anima/teacher.json") == 0);
            CHECK(nucleo_anima_sh_class("ui") == 1 && nucleo_anima_sh_class("input tap @3") == 0 && nucleo_anima_sh_class("home") == 0);
            CHECK(strstr(nucleo_anima_sh_grammar(true), "input tap @REF") && strstr(nucleo_anima_sh_grammar(false), "ui (schermo come testo"));
            system("mkdir -p anima_sd/home/shots");
            FILE *jf = fopen("anima_sd/home/shots/s.jpg", "wb");
            const unsigned char jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 0xFF, 0xD9 };
            fwrite(jpg, 1, sizeof jpg, jf); fclose(jf);
            FILE *tf = fopen("anima_sd/data/anima/teacher.json", "w");
            fputs("{\"provider\":\"local\",\"base\":\"http://192.168.1.10:11434/v1\",\"model\":\"qwen3.5:9b\"}", tf); fclose(tf);
            fakenet_clear();
            fakenet_add("/api/show", 200, "{\"capabilities\":[\"completion\",\"vision\",\"tools\",\"thinking\"]}");
            char cdesc[200];
            const int caps = nucleo_anima_model_caps(cdesc, sizeof cdesc);
            CHECK((caps & ANIMA_CAP_VISION) && (caps & ANIMA_CAP_TOOLS) && (caps & ANIMA_CAP_DETECTED) && strstr(cdesc, "qwen3.5:9b") && strstr(cdesc, "vision"));
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT see ~/shots/s.jpg\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Vedo le Impostazioni.\"}}]}");
            sr = ask("cosa vedi sullo schermo adesso?");
            CHECK(strstr(sr.reply, "Impostazioni") && strstr(sr.trace, "see"));
            CHECK(strstr(fakenet_last_post(), "image_url") && strstr(fakenet_last_post(), "data:image/jpeg;base64,/9j/"));
            // a photo attached to a question (Telegram): straight to the model that sees, never offline tiers
            CHECK(nucleo_anima_attach_image("~/shots/s.jpg"));
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"E' un ficus.\"}}]}");
            sr = ask("che pianta e'?");
            CHECK(strstr(sr.reply, "ficus") && !nucleo_anima_image_pending());
            CHECK(strstr(fakenet_last_post(), "image_url") && strstr(fakenet_last_post(), "immagine allegata"));
            // a text-only chat model + "vision_model": the helper describes, the chat model gets text
            tf = fopen("anima_sd/data/anima/teacher.json", "w");
            fputs("{\"provider\":\"local\",\"base\":\"http://192.168.1.10:11434/v1\",\"model\":\"llama3.1:8b\",\"vision_model\":\"qwen2.5vl:7b\"}", tf); fclose(tf);
            fakenet_clear();
            fakenet_add("/api/show", 200, "{\"capabilities\":[\"completion\",\"tools\"]}");
            CHECK(!(nucleo_anima_model_caps(cdesc, sizeof cdesc) & ANIMA_CAP_VISION) && strstr(cdesc, "vision helper: qwen2.5vl:7b"));
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT see /sdcard/nope.jpg\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"ACT see ~/shots/s.jpg\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Schermata Impostazioni: Wi-Fi spento.\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Il Wi-Fi risulta spento.\"}}]}");
            sr = ask("il wifi e' acceso? guarda lo schermo");
            CHECK(strstr(sr.reply, "spento") && strstr(sr.trace, "see(helper)"));
            CHECK(strstr(fakenet_last_post(), "described by qwen2.5vl:7b") && strstr(fakenet_last_post(), "Wi-Fi spento") && !strstr(fakenet_last_post(), "image_url"));
            CHECK(nucleo_anima_attach_image("~/shots/s.jpg"));
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"Un gatto rosso su un divano.\"}}]}");
            fakenet_add_once("/chat/completions", 200, "{\"choices\":[{\"message\":{\"content\":\"C'e' un gatto rosso.\"}}]}");
            sr = ask("cosa c'e' nella foto?");
            CHECK(strstr(sr.reply, "gatto") && strstr(fakenet_last_post(), "descritta da qwen2.5vl:7b") && !strstr(fakenet_last_post(), "image_url"));
            tf = fopen("anima_sd/data/anima/teacher.json", "w");
            fputs("{\"provider\":\"local\",\"base\":\"http://192.168.1.10:11434/v1\",\"model\":\"qwen2.5\"}", tf); fclose(tf);
            fakenet_clear();
            remove("anima_sd/data/anima/permissions.json");
            nucleo_anima_set_shell(nullptr);
            fakenet_clear();
        }
        nucleo_anima_set_net_mode(ANIMA_NET_HYBRID);

        // Telegram channel: token check, pairing with the device code, owner-only, send.
        {
            fakenet_clear();
            CHECK(!nucleo_anima_tg_set_token("not-a-token", false));
            const char *tok = "123456789:AAH-abcdefghijklmnopqrstuvwxyz0123456";
            fakenet_add("/getMe", 200, "{\"ok\":true,\"result\":{\"id\":1,\"is_bot\":true,\"username\":\"anima_test_bot\"}}");
            CHECK(nucleo_anima_tg_set_token(tok, false));
            anima_tg_status_t ts; nucleo_anima_tg_status(&ts);
            CHECK(ts.configured && ts.enabled && !ts.paired && !strcmp(ts.bot, "anima_test_bot") && strlen(ts.code) == 6);
            std::string code = ts.code;
            std::string upd = "{\"ok\":true,\"result\":[{\"update_id\":10,\"message\":{\"chat\":{\"id\":555},\"from\":{\"first_name\":\"Niki\"},\"text\":\"/pair " + code +
                "\"}},{\"update_id\":11,\"message\":{\"chat\":{\"id\":777},\"from\":{\"first_name\":\"Eve\"},\"text\":\"apri musica\"}},"
                "{\"update_id\":12,\"message\":{\"chat\":{\"id\":555},\"sticker\":{}}}]}";
            fakenet_add("/getUpdates", 200, upd.c_str());
            anima_tg_msg_t m[4]; char rep[300];
            int n = nucleo_anima_tg_poll(m, 4);
            CHECK(n == 2 && m[0].chat == 555 && !strcmp(m[0].from, "Niki"));
            CHECK(strstr(fakenet_last_url(), "offset=0"));
            CHECK(nucleo_anima_tg_accept(&m[0], false, rep, sizeof rep) == 0 && strstr(rep, "Collegato") && strstr(rep, "Niki"));
            CHECK(nucleo_anima_tg_accept(&m[1], false, rep, sizeof rep) == 0 && strstr(rep, "privato"));   // a stranger
            nucleo_anima_tg_poll(m, 4);
            CHECK(strstr(fakenet_last_url(), "offset=13"));                                        // acknowledged
            anima_tg_msg_t own = { 555, "Niki", "che ore sono" };
            CHECK(nucleo_anima_tg_accept(&own, false, rep, sizeof rep) == 1);                     // the owner: ANIMA answers
            anima_tg_msg_t again = { 999, "Mallory", "/pair 000000" };
            CHECK(nucleo_anima_tg_accept(&again, false, rep, sizeof rep) == 0 && strstr(rep, "sbagliato"));
            fakenet_add("/sendMessage", 200, "{\"ok\":true}");
            CHECK(nucleo_anima_tg_notify("Alle 11 il dentista") && strstr(fakenet_last_post(), "\"chat_id\":555") &&
                  strstr(fakenet_last_post(), "dentista"));
            // a photo with a caption: the largest size is picked, downloaded to ~/inbox, attachable
            fakenet_clear();
            fakenet_add("/getUpdates", 200, "{\"ok\":true,\"result\":[{\"update_id\":20,\"message\":{\"chat\":{\"id\":555},"
                "\"caption\":\"che pianta e'?\",\"photo\":[{\"file_id\":\"small\",\"file_size\":900},{\"file_id\":\"big\",\"file_size\":90000}]}}]}");
            n = nucleo_anima_tg_poll(m, 4);
            CHECK(n == 1 && !strcmp(m[0].photo, "big") && !strcmp(m[0].text, "che pianta e'?"));
            CHECK(nucleo_anima_tg_accept(&m[0], false, rep, sizeof rep) == 1);
            fakenet_add("/getFile", 200, "{\"ok\":true,\"result\":{\"file_path\":\"photos/file_1.jpg\"}}");
            fakenet_add("/file/bot", 200, "\xFF\xD8\xFF\xE0JPEGDATA");
            char ip[200] = "";
            CHECK(nucleo_anima_tg_fetch("big", ip, sizeof ip) == 0 && strstr(ip, "/home/inbox/tg-") && strstr(ip, ".jpg"));
            FILE *pf2 = fopen(ip, "rb"); CHECK(pf2 != nullptr); if (pf2) fclose(pf2);
            CHECK(nucleo_anima_attach_image(ip) && nucleo_anima_image_pending());
            nucleo_anima_attach_image(nullptr);
            CHECK(!nucleo_anima_image_pending() && !nucleo_anima_attach_image("anima_sd/none.jpg"));
            fakenet_add("/getMe", 200, "{\"ok\":true,\"result\":{\"id\":1,\"is_bot\":true,\"username\":\"anima_test_bot\"}}");
            nucleo_anima_tg_status(&ts);
            CHECK(ts.paired);
            nucleo_anima_tg_unlink();
            nucleo_anima_tg_status(&ts);
            CHECK(!ts.paired && strlen(ts.code) == 6);
            CHECK(!nucleo_anima_tg_notify("x"));
            nucleo_anima_tg_forget();
            nucleo_anima_tg_status(&ts);
            CHECK(!ts.configured);
            nucleo_anima_tg_request_token(tok);                                                  // from the web: queued
            nucleo_anima_tg_status(&ts);
            CHECK(ts.checking && !ts.configured);
            CHECK(nucleo_anima_tg_check_pending(false) == 1 && nucleo_anima_tg_check_pending(false) == -1);
            nucleo_anima_tg_status(&ts);
            CHECK(!ts.checking && ts.configured);
            nucleo_anima_tg_forget();
        }
        fakenet_clear();
        fakenet_online(0);
    }

    system("rm -rf anima_sd");
    return TEST_DONE("anima");
}
