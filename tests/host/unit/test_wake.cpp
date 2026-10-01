// nv_wake core on the host: the 48->16 kHz decimator, the wake state machine (fake detector) and the
// end-of-question detector.
#include "check.h"
#include <cmath>
#include <cstring>
#include <vector>
extern "C" {
#include "wake_core.h"
}

static int s_detect_calls, s_fire_at = -1, s_chunk = 480;
static bool s_open_ok = true;
static int fk_list(char n[][32], char l[][32], int max) {
    (void)max; strcpy(n[0], "wn9_hiesp"); strcpy(l[0], "Hi ESP"); strcpy(n[1], "wn9_alexa"); strcpy(l[1], "Alexa"); return 2;
}
static bool fk_open(const char *, int) { return s_open_ok; }
static int  fk_chunk(void) { return s_chunk; }
static bool fk_detect(const int16_t *) { return s_detect_calls++ == s_fire_at; }
static void fk_close(void) {}
static const wake_backend_t FAKE = { fk_list, fk_open, fk_chunk, fk_detect, fk_close };

static double rms(const int16_t *p, int n, int skip) {
    double a = 0; for (int i = skip; i < n; i++) a += (double)p[i] * p[i]; return std::sqrt(a / (n - skip));
}
static std::vector<int16_t> tone(double hz, int n) {
    std::vector<int16_t> v(n); for (int i = 0; i < n; i++) v[i] = (int16_t)(10000 * std::sin(2 * M_PI * hz * i / 48000.0)); return v;
}

int main()
{
    // Decimator: 3 in -> 1 out, a 1 kHz tone passes, a 12 kHz tone (would alias) is attenuated.
    {
        wake_decim_t d; wake_decim_reset(&d);
        std::vector<int16_t> out(4000);
        auto lo = tone(1000, 4800);
        int n = wake_decim_run(&d, lo.data(), (int)lo.size(), out.data());
        CHECK(n == 1600);
        const double pass = rms(out.data(), n, 20);
        CHECK(pass > 6000 && pass < 7600);          // ~7071 for a 10000-peak sine
        wake_decim_reset(&d);
        auto hi = tone(12000, 4800);
        n = wake_decim_run(&d, hi.data(), (int)hi.size(), out.data());
        CHECK(rms(out.data(), n, 20) < 700);        // > 20 dB down
        // streaming in odd pieces gives the same count
        wake_decim_reset(&d);
        int tot = 0; for (int i = 0; i < 4800; i += 7) tot += wake_decim_run(&d, lo.data() + i, std::min(7, 4800 - i), out.data());
        CHECK(tot == 1600);
    }

    // State machine
    {
        wake_core_t w; memset(&w, 0, sizeof w);
        wake_core_config(&w, true, "x", 1);
        CHECK(w.state == NV_WAKE_UNAVAILABLE && strstr(w.reason_en, "ESP-SR"));   // no backend
        w.be = &FAKE;
        wake_core_config(&w, true, "wn9_alexa", 1);
        CHECK(w.state == NV_WAKE_UNAVAILABLE && strstr(w.reason_en, "microphone"));
        w.mic_ok = true;
        wake_core_config(&w, true, "wn9_alexa", 5);
        CHECK(w.state == NV_WAKE_LISTENING && !strcmp(w.word, "wn9_alexa") && w.sens == 2 && w.nwords == 2);
        wake_core_config(&w, true, "missing", 1);
        CHECK(!strcmp(w.word, "wn9_hiesp"));                                       // falls back to the first
        std::vector<int16_t> buf(1440, 0);                                         // 30 ms @48k -> 480 @16k = 1 frame
        s_detect_calls = 0; s_fire_at = 2;
        wake_core_tick(&w, 1000);
        CHECK(!wake_core_feed(&w, buf.data(), 1440, false));
        CHECK(!wake_core_feed(&w, buf.data(), 1440, false));
        CHECK(wake_core_feed(&w, buf.data(), 1440, false) && w.state == NV_WAKE_HEARD && w.triggers == 1);
        s_fire_at = 3;                                                            // detector says yes again...
        CHECK(!wake_core_feed(&w, buf.data(), 1440, false) && w.triggers == 1);  // ...ignored while cooling down
        wake_core_tick(&w, 1000 + 2600);
        CHECK(w.state == NV_WAKE_LISTENING);
        s_fire_at = -1;
        CHECK(!wake_core_feed(&w, buf.data(), 1440, true) && w.state == NV_WAKE_PAUSED);   // mic busy
        CHECK(!wake_core_feed(&w, buf.data(), 1440, false) && w.state == NV_WAKE_LISTENING);
        wake_core_config(&w, false, "wn9_hiesp", 1);
        CHECK(w.state == NV_WAKE_OFF && !w.opened);
        s_open_ok = false;
        wake_core_config(&w, true, "wn9_hiesp", 1);
        CHECK(w.state == NV_WAKE_UNAVAILABLE && strstr(w.reason_en, "load"));
        s_open_ok = true;
        wake_core_free(&w);
    }

    // End of the question
    {
        nv_vad_state_t v; nv_vad_reset(&v);
        nv_vad_t r = NV_VAD_GO;
        for (int t = 0; t < 300; t += 50) r = nv_vad_step(&v, 10, 50);          // quiet room
        for (int t = 0; t < 1500; t += 50) r = nv_vad_step(&v, 60, 50);         // speech
        CHECK(r == NV_VAD_GO && v.spoke);
        int t = 0;
        while ((r = nv_vad_step(&v, 12, 50)) == NV_VAD_GO) t += 50;
        CHECK(r == NV_VAD_DONE && t >= 1000 && t <= 1200);
        nv_vad_reset(&v); t = 0;
        while ((r = nv_vad_step(&v, 8, 50)) == NV_VAD_GO) t += 50;
        CHECK(r == NV_VAD_NOTHING && t >= 4900);
        nv_vad_reset(&v);
        for (t = 0; t < 300; t += 50) nv_vad_step(&v, 10, 50);
        while ((r = nv_vad_step(&v, 70, 50)) == NV_VAD_GO) {}
        CHECK(r == NV_VAD_DONE && v.ms >= 12000);                               // nonstop talker: capped
    }
    return TEST_DONE("wake");
}
