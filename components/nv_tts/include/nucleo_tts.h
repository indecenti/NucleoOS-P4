// nucleo_tts — voce OFFLINE on-device per concatenazione di clip pre-renderizzate.
//
// PERCHE' COSI': il Cardputer (ESP32-S3, NIENTE PSRAM, ~512KB SRAM) non puo' sintetizzare
// fonemi in tempo reale — PicoTTS vuole ~1.1MB di RAM anche con risorse mmap (serve PSRAM),
// eSpeak ~120KB al limite assoluto. MA ANIMA offline NON e' un LLM generativo: e' una cascata
// di retrieval su un corpus FINITO, noto a build-time. Quindi pre-vocalizziamo offline (sul PC,
// con un TTS di qualita') il corpus + le parole a classe chiusa (numeri/date/unita'/connettivi),
// e a runtime CONCATENIAMO le clip. Voce naturale, RAM ~zero, offline e standalone puro. E' il
// gemello vocale di MOSAICO: "grounded by construction".
//
// FLUSSO: testo -> nucleo_tts_plan() (questo modulo, puro+testabile) -> sequenza di token CLIP/PAUSE
// -> nv_tts_say() (nv_tts.c) streama le clip PCM al codec via nv_audio. Le clip vivono su SD
// (/sdcard/data/tts/<lang>/index.bin + clips.pcm), NON nel flash (pref: niente asset bakeabili nell'immagine).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- planner (puro, host-compilabile: niente ESP/SD) -----------------------------------------

typedef enum {
    TTS_TOK_CLIP = 0,   // suona la clip identificata da `slug` (parola/frase/numero)
    TTS_TOK_PAUSE,      // inserisci `ms` di silenzio (punteggiatura/prosodia)
    TTS_TOK_UNKNOWN,    // parola NON coperta da nessuna clip -> l'enunciato non e' pronunciabile
                        // pulito: nv_tts.c lo sostituisce con una micro-pausa (o resta muto se troppi).
} tts_tok_kind_t;

typedef struct {
    tts_tok_kind_t kind;
    char slug[48];      // CLIP: slug della clip -> file /sd/data/tts/<lang>/<slug>.wav
    int  ms;            // PAUSE: durata silenzio in ms
    bool fallback;      // CLIP: 1 se prodotto da fallback (es. spelling lettera-per-lettera)
} tts_token_t;

// Callback: vero se lo slug ha una clip renderizzata. Permette al planner di degradare con grazia
// (spelling) su parole sconosciute. `ud` e' opaco (es. la lingua). I numeri/connettivi del
// "pacchetto obbligatorio" il planner li emette comunque (build_voice.py li genera sempre).
typedef bool (*tts_has_clip_fn)(const char *slug, void *ud);

// Pianifica un'enunciazione in una sequenza di token. Normalizza (lowercase, fold accenti), espande
// numeri/decimali/segno nei cardinali della lingua (lang "en" -> inglese, altrimenti italiano),
// prova un match a FRASE (greedy, MOSAICO-style) PRIMA del match a parola — cosi' le frasi comuni
// hanno precedenza — e per le parole ignote ripiega sullo spelling. Ritorna i token scritti (<= max).
int nucleo_tts_plan(const char *text, const char *lang,
                    tts_has_clip_fn has_clip, void *ud,
                    tts_token_t *out, int max);

// ---- velocita' di lettura (zero-RAM: rate dell'header WAV in uscita) --------------------------
// La voce si accelera/rallenta cambiando il SAMPLE-RATE dichiarato nell'header del WAV assemblato:
// il DAC scandisce gli STESSI campioni piu' in fretta (clip + pause si accorciano insieme) — nessun
// buffer, nessun sample toccato, costo RAM/CPU ZERO. Tape-speed: a >100% sale leggermente il pitch
// (accettabile per "un po' piu' veloce"). La velocita' e' un intero PERCENTUALE (100 = naturale).
#define TTS_SPEED_MIN 70     // 0.70x (piu' lento)
#define TTS_SPEED_MAX 160    // 1.60x (piu' veloce)
#define TTS_SPEED_DEF 110    // default: un filo piu' veloce del naturale (l'utente lo trovava "lentino")
#define TTS_SPEED_STEP 5     // passo consigliato per gli slider/▲▼

// Clampa il percentuale a [TTS_SPEED_MIN, TTS_SPEED_MAX]. Pura/testabile.
int nucleo_tts_speed_clamp(int pct);

#ifdef __cplusplus
}
#endif
