// ANIMA engine, end to end on the host: the real cascade (nucleo_anima_query) with the network
// stubbed offline and the SD at ./anima_sd. Checks the L0 commands, the tools, the solver and the
// session memory answer what they say - the device behaviour a user sees in the ANIMA app.
#include "check.h"
#include <cstring>
#include <cstdlib>
#include <string>
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
        CHECK(nucleo_anima_act_from_llm("ACT add_event 1 09:30 dentista", false, &a) && !strcmp(a.intent, "add_event") &&
              !strcmp(nucleo_anima_tool_content(), "off=1;time=09:30;text=dentista"));
        CHECK(nucleo_anima_act_from_llm("ACT create_file spesa.txt | latte, pane", false, &a) &&
              !strcmp(a.arg, "/data/Documents/spesa.txt") && !strcmp(nucleo_anima_tool_content(), "latte, pane"));
        CHECK(!nucleo_anima_act_from_llm("ACT open_app rm-rf", false, &a));
        CHECK(!nucleo_anima_act_from_llm("ACT set_volume 300", false, &a));
        CHECK(!nucleo_anima_act_from_llm("ACT create_file ../boot.bin | x", false, &a));
        CHECK(!nucleo_anima_act_from_llm("ACT format_sd", false, &a));
        CHECK(!nucleo_anima_act_from_llm("Ecco: ACT open_app calc", false, &a));
        CHECK(strstr(nucleo_anima_act_grammar(false), "ACT open_app") != nullptr);
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
        nucleo_anima_set_net_mode(ANIMA_NET_HYBRID);

        // Speech-to-text: the home Whisper server first (no key), the cloud only when it fails.
        FILE *wav = fopen("anima_sd/v.wav", "wb"); fputs("RIFFfakeaudio", wav); fclose(wav);
        t = fopen("anima_sd/data/anima/teacher.json", "w");
        fputs("{\"provider\":\"groq\",\"key\":\"gsk_test\",\"stt_url\":\"http://192.168.1.20:8080/inference\"}", t);
        fclose(t);
        char txt[128], lg[8];
        fakenet_add("192.168.1.20:8080/inference", 200, "{\"text\":\" Ciao, come stai?\"}");
        CHECK(nucleo_anima_transcribe("anima_sd/v.wav", "auto", txt, sizeof txt, lg, sizeof lg) > 0 && strstr(txt, "Ciao"));
        CHECK(strstr(fakenet_last_url(), "192.168.1.20") && strstr(fakenet_last_post(), "RIFFfakeaudio") &&
              strstr(fakenet_last_post(), "name=\"file\""));
        fakenet_clear();                                      // home server down -> the cloud key answers
        fakenet_add("api.groq.com/openai/v1/audio/transcriptions", 200, "{\"text\":\"Hello there\",\"language\":\"english\"}");
        CHECK(nucleo_anima_transcribe("anima_sd/v.wav", "auto", txt, sizeof txt, lg, sizeof lg) > 0 &&
              !strcmp(txt, "Hello there") && !strcmp(lg, "en"));
        nucleo_anima_set_net_mode(ANIMA_NET_LOCAL);           // LAN-only: never the cloud
        CHECK(nucleo_anima_transcribe("anima_sd/v.wav", "auto", txt, sizeof txt, lg, sizeof lg) < 0);
        nucleo_anima_set_net_mode(ANIMA_NET_HYBRID);
        fakenet_clear();
        fakenet_online(0);
    }

    system("rm -rf anima_sd");
    return TEST_DONE("anima");
}
