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

    // Reminders keep the WHEN out of the text
    expect("ricordami domani alle 9 di chiamare Marco", "add_event", nullptr, "domani alle 09:00");

    // Profile memory
    expect("mi chiamo Niki", "profile", nullptr, "Niki");
    expect("come mi chiamo?", "profile", nullptr, "Niki");

    // Honest miss offline (no knowledge pack, no network): no fabricated answer
    anima_result_t r = ask("chi era Alan Turing?");
    CHECK(r.tier == ANIMA_TIER_NONE || r.confidence == 0);

    system("rm -rf anima_sd");
    return TEST_DONE("anima");
}
