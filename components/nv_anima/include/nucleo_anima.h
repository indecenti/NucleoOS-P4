// ANIMA — on-device natural-language assistant, offline first.
//
// One entry point (nucleo_anima_query) runs the cascade: L0 commands / tools / math solver ->
// L1 semantic retrieval over the knowledge cards (SD) -> HDC/KGE deduction over learned facts ->
// L2 span-stitch -> optional online tiers (Wikipedia cards, a cloud or LAN "teacher"). Each tier
// abstains rather than guess; a miss is an honest "non lo so". Callers serialize on the spine
// gate (nucleo_anima_try_lock / _unlock): the native app's worker and the web handler.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"   // CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC (TLS heap bars below)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Heap bars before risking an outbound TLS handshake — the ONE definition shared by both pre-TLS
// gates (the httpd pre-gate in nv_web.cpp and the fetch guard in nucleo_anima_online.c).
// TWO independent constraints (measured: the original crash was TOTAL-heap exhaustion, not
// contiguity — the 16 KB rx buffer fit a 20 KB block, but the handshake's ~35 KB SUM of small
// allocations overran a ~30 KB total): a contiguous block large enough for the SSL_IN_CONTENT_LEN
// rx buffer, AND enough TOTAL free for the whole handshake. Tune via /api/heap if too eager.
// MIN_BLOCK: the rx buffer is realloc'd to the incoming record size (MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH),
// so the handshake's real peak contiguous need is the cert-chain record (~4-6 KB), NOT the 16 KB
// SSL_IN_CONTENT_LEN cap. Measured on this no-PSRAM device: a fresh heap gives a ~21 KB largest block,
// but the FIRST TLS handshake fragments it to a stable ~17 KB — so an 18 KB gate permanently blocked
// EVERY online turn after the first (the "dopo una domanda non risponde piu" bug). 16 KB + 512 margin
// admits that 17 KB block while still comfortably fitting any real handshake record; MIN_FREE below
// stays the true OOM guard (the original crash was total-heap exhaustion, not contiguity).
#if CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
// mbedTLS allocates its session, record buffers and the CA-bundle parse in PSRAM
// (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC): the handshake no longer draws on internal SRAM, so the
// bars only keep room for what still lands there (socket, lwIP control blocks, small libc allocs).
// The measurements below are those of the internal-SRAM build and are kept for history.
#define NUCLEO_TLS_MIN_BLOCK (4 * 1024)
#define NUCLEO_TLS_MIN_FREE  (16 * 1024)
#else
#define NUCLEO_TLS_MIN_BLOCK (10 * 1024)   // largest contiguous internal block: with SSL_IN_CONTENT_LEN now 8 KB the
                                           // peak rx record needs <9 KB contiguous, so a 10 KB block is ample and the
                                           // fragmented ~17 KB steady-state passes with room to spare.
#define NUCLEO_TLS_MIN_FREE  (34 * 1024)   // total internal free for the full handshake (~34 KB measured SUM: CA-bundle
                                           // parse + session + lwIP). Was 38 KB for a 4 KB margin, but on a tight unit the
                                           // idle free sits at ~36 KB (L1 + canvas + httpd + Wi-Fi) and oscillates BELOW 38,
                                           // so the gate PERMANENTLY refused transcribe/summarize ("AI fallita" even though
                                           // Groq + key were fine — confirmed by `skip POST: free 36796<38912`). 34 KB = the
                                           // real peak: the device survives the handshake at near-OOM (min_free seen ~340 B,
                                           // no crash), and the wait-and-retry in http_post_json + the recorder's own retries
                                           // cover the rest. Lower this only with /api/heap evidence.
#endif

// Which cascade tier produced the result.
typedef enum {
    ANIMA_TIER_NONE = 0,   // nothing matched with enough confidence
    ANIMA_TIER_COMMAND,    // L0: command / static FAQ hit
    ANIMA_TIER_FACT,       // L1 retrieval / HDC-KGE deduction / learned facts
    ANIMA_TIER_STITCH,     // L2: MOSAICO span-stitch of two L1 cards
    ANIMA_TIER_REMOTE,     // online: Wikipedia card or a cloud / LAN teacher
} anima_tier_t;

// What the caller should do with a result.
typedef enum {
    ANIMA_ACT_NONE = 0,
    ANIMA_ACT_LAUNCH,      // open an app: arg = app id (e.g. "photo-viewer")
    ANIMA_ACT_SYSTEM,      // report live state: arg = "battery" | "time" | "storage"
    ANIMA_ACT_ANSWER,      // static answer already in `reply`
    ANIMA_ACT_TOOL,        // run a tool: intent = tool name (e.g. "create_file"), arg = parameter
} anima_action_t;

// One more device action of a compound request ("chiudi la musica e apri le note"). The primary action
// stays in anima_result_t.action/intent/arg; the rest follow here, in the order they were asked, and
// every surface runs them through nv_anima_os_run (it executes the primary TOOL, then these).
#define ANIMA_PLAN_MAX 3
typedef struct {
    char intent[24];       // a TOOL intent (set_volume, set_brightness, close_app, add_event, create_file)
    char arg[64];
} anima_step_t;

typedef struct {
    anima_tier_t   tier;
    anima_action_t action;
    char intent[24];       // intent id, for logging/telemetry
    char arg[64];          // app id / system key / routed file path (empty if none)
    char reply[1024];      // human-facing text (web shows all; native clips on render). L0/L1/card
                           // replies stay well under 256; online bios ~360; a CODE snippet from the
                           // online model uses the larger budget (fenced markdown, web renders it).
    int  confidence;       // 0..100
    int  budget;           // micro-thought: clusters probed by L1 (0 if not used)
    int  from_memory;      // micro-thought: 1 if resolved from utility memory (a follow-up)
    // --- agentic controller (deterministic, no generation) ---
    int  awaiting;         // 1 if this reply is a question expecting a follow-up (slot/clarify)
    char state[12];        // FSM state that produced this turn: idle|slot|clarify|followup|tool
    char corrected[64];    // typo-corrected query actually understood ("" if input was clean)
    char trace[112];       // visible reasoning trace: the steps this turn took (" · " joined)
    // --- conversational focus (set by the deductive tier so a follow-up can re-aim it) ---
    char subject[48];      // the entity the reasoner anchored ("Albert Einstein"); "" if none
    char relation[24];     // structured relation token it used ("born"|"capital"|"located_in"|...); "" if none
    // --- fallback ---
    int  degraded;         // 1 = the mode wanted the language model but it was not usable or did not answer:
                           //     the device (+ the web sources, if there is internet) answered instead
    // --- compound request: more TOOL actions after the primary one (docs/ANIMA_MODES.md, "Piani") ---
    int  nsteps;
    anima_step_t steps[ANIMA_PLAN_MAX];
} anima_result_t;

// True when a result carries device work for nv_anima_os_run: a TOOL, or a plan's extra steps.
static inline bool nucleo_anima_has_tool_work(const anima_result_t *r)
{
    return r && (r->action == ANIMA_ACT_TOOL || r->nsteps > 0);
}

// Load the command pack for `lang` ("it" for now). Idempotent.
esp_err_t nucleo_anima_init(const char *lang);

// Run the cascade on a UTF-8 input line. Understands IT+EN; replies in `lang`
// ("en" -> English, anything else -> Italian). Always returns (tier NONE if unsure).
anima_result_t nucleo_anima_query(const char *input, const char *lang);
// Who is asking ("screen", "web", "tg", "rule"): set before nucleo_anima_query (under the engine
// gate). A pending "ask" confirmation is accepted only from the origin that raised it, so a "yes"
// on Telegram or an automation can never approve an action proposed on the screen. Returns the
// previous origin (static storage) so a nested caller can restore it.
const char *nucleo_anima_set_origin(const char *origin);

// Lightweight cumulative query telemetry for the diagnostics surface (/api/diag, Log Viewer). These
// are plain u32 counters bumped once at the single convergence point of nucleo_anima_query() — no SD,
// no alloc, no hot-path cost — so the web side can compute an honest ANIMA health picture (abstain
// rate, tier mix, how often the cloud teacher was reached) without instrumenting the cascade itself.
typedef struct {
    uint32_t queries;        // total nucleo_anima_query() calls since boot
    uint32_t t_none;         // tier NONE  -> honest miss / abstain (the rate that flags a weak corpus)
    uint32_t t_command;      // tier L0 command/FAQ hits
    uint32_t t_fact;         // tier L1/KGE frozen-fact hits
    uint32_t t_stitch;       // tier L2 MOSAICO span-stitch
    uint32_t t_remote;       // tier L3 cloud teacher answers
    int      last_conf;      // confidence of the most recent answer (0..100)
    char     last_intent[24];// intent id of the most recent answer (for the live tail)
} anima_diag_t;

// Snapshot the counters above. Cheap, lock-free read (u32 stores are atomic enough for telemetry).
void nucleo_anima_diag(anima_diag_t *out);

// Spine gate: serialize nucleo_anima_query() across its two callers (web handler + native worker) so
// only ONE cascade owns the shared L1/session state at once. The web handler try-locks (returns a lean
// 503 "busy" on contention, never blocking the server); the native worker polls briefly then answers
// "busy". Without this, two concurrent queries free/read the same L1 buffers -> use-after-free + reboot.
bool nucleo_anima_try_lock(void);
void nucleo_anima_unlock(void);

// L1 index RAM management, exposed to the web layer so server-side TLS fetchers (online ANIMA tiers,
// /api/proxy, /api/llm) can reclaim the L1 index for the mbedTLS handshake's contiguous-heap need on
// this PSRAM-less chip. _unload() frees it (reloads transparently on the next query); _heap_bytes()
// reports how much that reclaim would free right now (0 when already unloaded).
void   nucleo_anima_l1_unload(void);
// Guarded reclaim for callers OUTSIDE the cascade: unload only if no query is running (try-locks the
// spine gate). Returns false (and frees nothing) when ANIMA is busy — skip the reclaim, don't corrupt.
bool   nucleo_anima_l1_unload_if_idle(void);
size_t nucleo_anima_l1_heap_bytes(void);
// Guarded flush of the L1 PSRAM file mirrors (P4: up to 12 MB of SD shards cached in PSRAM).
// Frees nothing when a query is running. Returns bytes freed. Wired to the memory broker so
// RAM-heavy apps (camera) win over the rebuildable cache.
size_t nucleo_anima_l1_cache_flush_if_idle(void);

// ── Offline L1/HDC "programmatic brain" serving policy (RAM optimization) ──────────────────────
// L1 (this semantic index + the HDC reasoner it feeds) is ANIMA's heaviest RAM tenant. By default
// (AUTO) it STANDS DOWN whenever a stronger brain is already answering — a cloud teacher (Grok/Claude
// key + connectivity) or a browser-hosted local LLM — so the index never loads on an online turn and
// the freed ~18-31 KB goes to the TLS handshake instead. The user can force it back ON (always serve,
// e.g. to test the offline brain) or OFF (never serve) from the ANIMA web app or the native ANIMA app.
enum { ANIMA_L1_AUTO = 0, ANIMA_L1_ON = 1, ANIMA_L1_OFF = 2 };
bool nucleo_anima_l1_serving(void);               // would L1 serve the next query under the current policy?
int  nucleo_anima_l1_get_mode(void);              // ANIMA_L1_AUTO | _ON | _OFF
void nucleo_anima_l1_set_mode(int mode);          // user override (web/native); frees the index if it turns off
void nucleo_anima_l1_set_online_brain(bool on);   // orchestrator: a cloud teacher WITH a key is reachable this turn

// Network policy (persisted by the apps as "anima.net"):
//   OFF    offline: the device alone, nothing goes on the network.
//   LOCAL  the device + a language model server on the LAN (Ollama, LM Studio, llama.cpp, nucleomind)
//          as fallback; nothing ever leaves the local network.
//   HYBRID (default) the device first, then Wikipedia / Wikidata, then the configured language model
//          (LAN or cloud) as the last resort.
//   LLM    the configured language model answers first (agent); when it is missing or silent the
//          device's own tiers answer, commands included, plus the web sources if there is internet.
enum { ANIMA_NET_OFF = 0, ANIMA_NET_LOCAL = 1, ANIMA_NET_HYBRID = 2, ANIMA_NET_LLM = 3 };
void nucleo_anima_set_net_mode(int mode);
int  nucleo_anima_get_net_mode(void);

// ── What a turn can really use (docs/ANIMA_MODES.md) ───────────────────────────────────────────
// The mode is a WISH; each turn runs with what is there now. Three resources, checked per turn:
//   network  the device has an IP and the mode allows the network (not OFF)
//   web      internet sources (Wikipedia / Wikidata / weather / news / exchange rates…): network, not LOCAL
//   model    a language model configured for this mode (LOCAL: a LAN server only) and not on cooldown
//            after a failure (bad key 10 min, quota 1 min, unreachable 15 s)
// The ladder never breaks: with no model (none configured, on cooldown, or it does not answer this
// turn) every mode — LLM included — runs the device tiers (L0 commands and tools, solver, L1
// knowledge) plus the web sources when there is internet. A command typed in LLM mode still runs.
typedef enum {
    ANIMA_RUN_DEVICE = 0,  // device only: L0 / tools / solver / L1 / HDC
    ANIMA_RUN_WEB,         // device + internet sources, no model
    ANIMA_RUN_LOCAL_LLM,   // LOCAL: device + LAN model as the last resort (nothing leaves the LAN)
    ANIMA_RUN_HYBRID,      // HYBRID: device + internet sources + model as the last resort
    ANIMA_RUN_AGENT,       // LLM: the model owns the turn; device (+ web) only when it fails
} anima_run_t;

typedef struct {
    int         mode;      // configured ANIMA_NET_*
    bool        network;
    bool        web;
    bool        model;
    bool        degraded;  // the mode relies on a model (LLM) and none is usable: running a lower rung
    anima_run_t run;       // the rung the next turn starts on
} anima_route_t;

// Snapshot the route the next turn would take. Reads the teacher config (SD): not on a hot path.
void nucleo_anima_route(anima_route_t *out);
// Short human label of a route ("Agente (modello)", "Dispositivo + web", ...).
const char *nucleo_anima_route_label(const anima_route_t *r, bool en);
// A language model is configured for the current mode, the device is online, and its first-choice
// endpoint is not on cooldown. False means: don't wait on it, go straight to the device tiers.
bool nucleo_anima_model_usable(void);
// The same cascade as nucleo_anima_query, for a turn whose model call ALREADY failed elsewhere (the
// conversation chat): the model is not tried again, the device and web tiers answer. Caller holds
// the spine gate, like nucleo_anima_query.
anima_result_t nucleo_anima_query_no_model(const char *input, const char *lang);

// Why the network tiers failed THIS turn, as a short user-facing line ("chiave API non valida",
// "quota esaurita", "server non raggiungibile", ...), or "" when no cloud call failed.
const char *nucleo_anima_online_fail_note(bool en);
void nucleo_anima_online_turn_begin(void);   // forget the previous call's failure (start of a turn)

// The models the active teacher's server lists (GET <base>/models), as a JSON array of ids in `out`.
// Count, or -1 (no teacher / no answer). A network call: workers or the httpd task only.
int nucleo_anima_teacher_models(char *out, int cap);

// LAN model-server discovery (nucleo_anima_scan.c): a background sweep of the station's /24 for
// Ollama / LM Studio / llama.cpp. start: rate-limited (10 min; `force` 45 s), returns at once.
// models: the chat models found, JSON [{"m":model,"b":base,"h":host,"k":kind}]; count or -1.
// set_server: point the teacher at a found server (provider "local", base + model), sealed write.
void nucleo_anima_scan_start(bool force);
bool nucleo_anima_scan_busy(void);
int  nucleo_anima_scan_models(char *out, int cap);
bool nucleo_anima_teacher_set_server(const char *base, const char *model);
bool nucleo_anima_teacher_base(char *base, int cap);   // the active teacher's base URL ("" if none)

// Relay one HTTP request for the web surfaces' /api/llm (see nucleo_anima_online.c): `hdr` holds up to
// 3 (name, value) pairs. Body length (any status) or -1; *out is heap (caller frees), *status the HTTP
// status. Network call: workers only. nucleo_anima_url_is_local: a private / loopback / .local host.
int  nucleo_anima_http_relay(const char *url, const char *method, const char *const hdr[6], const char *body,
                             int max_bytes, char **out, int *status);
bool nucleo_anima_url_is_local(const char *url);

// Record a file as the current context for follow-ups (the executor calls this once a
// create_file actually leaves a file on disk, or when the named file already exists).
void nucleo_anima_note_file(const char *path);

// Cloud speech-to-text (no on-device ASR model): streams the audio at `path` to the Whisper endpoint;
// lang_hint="auto" lets Whisper detect the spoken language (returned in out_lang), else forces it.
// Returns the transcript length in out_text, or -1 (no key / offline / error). Used by the native
// ANIMA app's voice input.
int nucleo_anima_transcribe(const char *path, const char *lang_hint, char *out_text, int tcap, char *out_lang, int lcap);
// Where voice would be transcribed now: 1 home server (where = host:port), 2 cloud (where = provider),
// 0 nowhere configured. No network call.
int nucleo_anima_stt_route(char *where, int cap);

// Cloud availability, so a UI can show honest status before attempting a network feature.
bool nucleo_anima_online_available(void);     // online tier enabled AND the device currently has an IP
bool nucleo_anima_teacher_configured(void);   // a chat-teacher key (any provider — Claude or OpenAI-compatible) is set
// Report the ACTIVE chat teacher without exposing the key: provider ("anthropic"|"openai") + model.
// Returns true iff a key is configured. Lets a UI show "Claude (sonnet-4-6)" vs "Grok (llama-3.1-8b)".
bool nucleo_anima_teacher_info(char *provider, int pcap, char *model, int mcap);

// Content channel for multi-step "compose THEN act": when the controller computes a payload too
// large for the 64-byte arg field (e.g. a file body composed from a calculation or a literal text
// clause), it stashes it here and the EXECUTOR writes it. Returns "" when the last turn produced no
// payload (then create_file makes an empty file, the legacy behavior). Valid until the next query.
const char *nucleo_anima_tool_content(void);
// LLM tool-calling: the action grammar for the system prompt, and the validator that turns a model's
// "ACT <tool> <args>" line into a LAUNCH/TOOL result (1) or leaves it an answer (0).
const char *nucleo_anima_act_grammar(bool en);
int nucleo_anima_act_from_llm(const char *text, bool en, anima_result_t *r);
// Skills: SD know-how files (/data/anima/skills/*.md) matched on the question's trigger phrases.
// _prompt fills the model's skill block (0 = none active), _offline the skill's offline answer (1/0),
// _list the installed skill names (returns how many).
int nucleo_anima_skills_prompt(const char *q, bool en, char *out, int cap);
int nucleo_anima_skills_offline(const char *q, char *out, int cap);
int nucleo_anima_skills_list(char *out, int cap);
// The workspace, OpenClaw-style files on the SD the user edits by hand (/data/anima/):
// SOUL.md + USER.md -> the model's system block (_workspace_prompt, 0 = none), HEARTBEAT.md -> the
// proactive checklist (_heartbeat_list), permissions.json -> what a model's ACT line may do without
// asking: 0 allow, 1 ask (a yes/no turn first), 2 deny (_permission).
int nucleo_anima_workspace_prompt(bool en, char *out, int cap);
int nucleo_anima_heartbeat_list(char *out, int cap);
// The model's ACT remember: one fact into the user memory (memory.jsonl, nucleo_anima_conv.h — the
// store the web chat and its memory page use too). 1 = saved (or already known).
int nucleo_anima_memory_add(const char *fact);
// Remove the MEMORY.md lines containing every word of `what`. Returns how many.
int nucleo_anima_memory_forget(const char *what);
// The workspace prompt with MEMORY.md recalled for this question (related lines, recent ones, #labels).
int nucleo_anima_workspace_prompt_q(bool en, const char *query, char *out, int cap);
// ANIMA's shell tool: the OS registers an executor that runs one Linux-like command line headless and
// returns its exit status (output in `out`; -1 busy). The model then works in steps ("ACT sh ls /data"),
// seeing each output. Read-only lines run at once; the rest follows permissions.json "sh" (default
// ask; "mode":"auto" lets it run); full-screen commands are refused (_sh_class: 1 / 0 / -1).
void nucleo_anima_set_shell(int (*exec)(const char *line, char *out, int cap));
bool nucleo_anima_has_shell(void);
const char *nucleo_anima_sh_grammar(bool en);   // the prompt lines for it ("" without a shell)
// File tools (ACT write / ACT edit, multi-line <<< >>> blocks; permission "write", default ask).
// Runs one and writes the result for the model to `res`; 0 = not a file tool.
int nucleo_anima_file_tool(const char *content, bool en, char *res, int cap);
// True for a file action line (write, edit, or create_file given a path) that nucleo_anima_file_tool runs.
bool nucleo_anima_is_file_act(const char *c);
// The one "write a temp file, then rename" path (ENGINEERING_RULES §5), shared with the OS layer:
// `n` bytes go to "<path>.tmp", which then replaces `path` (see a_commit_tmp). True when `path`
// holds the new data. a_mkdirs creates every parent directory of `path` (mkdir -p of its dirname).
bool a_write_atomic(const char *path, const void *data, size_t n);
void a_mkdirs(const char *path);
int  nucleo_anima_sh_class(const char *line);
// Autonomous mode (permissions.json "mode":"auto"): actions that would ask run at once; deny holds.
bool nucleo_anima_auto_mode(void);
bool nucleo_anima_set_auto_mode(bool on);
// What the chat model can do (nucleo_anima_model_caps): bits below. DETECTED = the server said so
// (Ollama /api/show), otherwise the model family decided.
#define ANIMA_CAP_VISION    1
#define ANIMA_CAP_TOOLS     2
#define ANIMA_CAP_THINKING  4
#define ANIMA_CAP_DETECTED  8
// The active chat model's capabilities + a one-line description for /caps ("qwen3.5:9b: vision,
// tools (Ollama)"); also says which vision helper (teacher.json "vision_model") is set. -1 = none.
int nucleo_anima_model_caps(char *desc, int cap);
// Attach a JPEG/PNG (absolute path, or ~/...) to the NEXT question: a model that sees gets it with
// that request, otherwise the vision helper describes it first. NULL clears. False: unreadable.
bool nucleo_anima_attach_image(const char *path);
bool nucleo_anima_image_pending(void);
// Timers and alarms (nucleo_anima_time.c), offline. The tool: 1 = handled (r filled), 0 = not one.
int nucleo_anima_timer_tool(const char *raw, bool en, long long now_epoch, anima_result_t *r);
// For the OS, once a second: how many timers/alarms are due at `now` (removed from the store); the
// first one's label and whether it is an alarm.
int nucleo_anima_timers_due(long long now, char *label, int cap, bool *alarm);
// The earliest pending timer/alarm (epoch), 0 = none. Cached: reads the SD only after a change.
long long nucleo_anima_timers_next(void);
// The skills as a catalog for the agent's prompt (Agent Skills progressive disclosure). Returns length.
int nucleo_anima_skills_catalog(bool en, char *out, int cap);
// Automations (nucleo_anima_rules.c, ESP-Claw's event router): /data/anima/rules.json. The OS posts
// events; the matching rules run their actions (caller holds the engine gate). Returns 0 no rule,
// 1 matched, 2 consumed (reply = the ack or the last output, for a message event).
typedef struct { char type[16]; char key[48]; char text[400]; int wday; char from[48]; } anima_event_t;
// Home Assistant state changes ("ha_state" rules: match.event_key = entity, optional "to"/"from").
// _ha_watch: the Jinja template that returns "entity=state" lines for the watched entities (0 = no
// such rule: nothing to poll). _ha_states: the template's output; changes since the previous call
// fire the rules (the first call only remembers). Returns how many changes fired rules.
int nucleo_anima_rules_ha_watch(char *tpl, int cap);
int nucleo_anima_rules_ha_states(const char *resp, bool en);
int nucleo_anima_rules_handle(const anima_event_t *ev, bool en, char *reply, int cap);
int nucleo_anima_rules_add(const char *json, bool en, char *msg, int cap);   // 1 saved
int nucleo_anima_rules_delete(const char *id);                              // "*" = all; how many
int nucleo_anima_rules_list(bool en, char *out, int cap);                   // how many
void nucleo_anima_rules_set_notifier(void (*fn)(const char *title, const char *text));
// Agent mode: 0 normal, 1 auto, 2 plan ("mode":"plan": read-only, what would change something is denied).
// The context meter: tokens the last chat turn used (server-counted, else ~chars/4) and the model's
// window (Ollama /api/show when detected, else its family). 0/0 before the first model turn.
void nucleo_anima_ctx_stats(int *used, int *max);
// STOP: the turn in flight gives up at its next check (before/after each HTTP attempt to a cloud API
// or an Ollama/LAN server, between agent steps); its answer is discarded and no action it proposed
// runs. Any task may call it. Cleared when the next turn begins. A request already on the wire
// finishes in the background (bounded by its timeout) and is dropped.
void nucleo_anima_cancel(void);
bool nucleo_anima_cancelled(void);
void nucleo_anima_cancel_reset(void);   // a new job (not a query: /compact, the model list...) starts clean
void nucleo_anima_ctx_saved(int tokens);   // a compaction freed ~tokens: the meter drops until the next turn

// Context compaction (Claude Code /compact): older turns folded into ONE structured summary that rides
// in the system block; the last turns stay verbatim. Auto before a turn at >= 80% of the window
// (or a large backlog of turns that left the ring). Network calls: the worker, under the spine gate.
typedef struct { int count; int turns; int saved_tokens; } anima_compact_info_t;
int  nucleo_anima_compact(const char *focus, bool en);   // 1 compacted, 0 nothing to fold, -1 failed/offline
void nucleo_anima_set_autocompact(bool on);
bool nucleo_anima_autocompact(void);
bool nucleo_anima_compacting(void);                       // a compaction call is in flight (spinner)
void nucleo_anima_compact_info(anima_compact_info_t *out);
const char *nucleo_anima_session_summary(void);          // "" = none
// The workspace (a folder under /sdcard, "~/..." accepted): the shell starts the next command there
// and the model's grammar names it. false = refused (outside the card, quotes, ".."). Default ~.
bool nucleo_anima_set_workspace(const char *path);
const char *nucleo_anima_workspace(void);
int nucleo_anima_agent_mode(void);
bool nucleo_anima_set_agent_mode(int mode);
int nucleo_anima_permission(const char *tool);
// Heartbeat: one quiet look at HEARTBEAT.md with the model. `ctx` = live facts from the OS (time,
// today's agenda...). 1 = something needs the user (out = a short notification), 0 = all fine
// (the model said HEARTBEAT_OK), no checklist, the mode forbids a model, or the call failed.
int nucleo_anima_heartbeat(const char *ctx, bool en, char *out, int cap);

// Telegram channel (nucleo_anima_telegram.c): a bot the owner pairs with a 6-digit code; the OS task
// polls, runs the owner's messages through ANIMA and sends the answer back.
typedef struct { long long chat; char from[32]; char text[400]; char photo[100]; } anima_tg_msg_t;   // photo: file_id or ""
// Download a Telegram file (a photo's file_id) to ~/inbox; path gets the absolute path. 0 = ok.
int nucleo_anima_tg_fetch(const char *file_id, char *path, int cap);
typedef struct { bool configured, enabled, paired, checking; char bot[48]; char code[8]; char error[96]; } anima_tg_status_t;
void nucleo_anima_tg_status(anima_tg_status_t *st);
const char *nucleo_anima_tg_pair_code(void);
int  nucleo_anima_tg_set_token(const char *token, bool en);   // checks it with getMe, saves; 1 = ok
void nucleo_anima_tg_set_enabled(bool on);
// From a context that must not open TLS (the web server): queue a token; the channel task checks it
// (nucleo_anima_tg_check_pending: 1 ok, 0 refused, -1 nothing queued) and the status says how it went.
void nucleo_anima_tg_request_token(const char *token);
int  nucleo_anima_tg_check_pending(bool en);
void nucleo_anima_tg_unlink(void);                             // forget the paired chat (new code)
void nucleo_anima_tg_forget(void);                             // forget token and chat
int  nucleo_anima_tg_poll(anima_tg_msg_t *m, int max);         // new text messages (0..max), -1 = failed
// 1 = the owner's message: run it through ANIMA. 0 = handled here (pairing, help, refusal): send `reply`.
int  nucleo_anima_tg_accept(const anima_tg_msg_t *m, bool en, char *reply, int cap);
bool nucleo_anima_tg_send(long long chat, const char *text);
bool nucleo_anima_tg_notify(const char *text);                 // to the paired chat, if enabled

// Overflow reply channel: a reply too long for result.reply[1024] (a multi-line CODE snippet from the
// online model) is stashed here on the heap by the online tier; the web layer serves THIS verbatim when
// present (cJSON handles long strings) instead of the clipped struct field. NULL/"" = use result.reply.
const char *nucleo_anima_long_reply(void);
void        nucleo_anima_set_long_reply(const char *s);

// Close the agentic loop: the executor reports the real outcome of the last action back to
// the controller (e.g. create_file succeeded / was blocked / already existed). `ok` drives
// whether the action enters working memory and clears any pending slot. Best-effort.
void nucleo_anima_observe(const char *intent, bool ok);

// Forget the conversational state (pending slot, last app/file/topic, working-memory ring).
// The session otherwise persists across reboots on the SD. Used by "pulisci conversazione".
void nucleo_anima_reset_session(void);

// Switch the conversation the model sees (window + compacted summary) to the one stored at
// ctx_path (a context.json; absent = a new, empty conversation). The current one is saved to its own
// file first and the working memory starts over. Before the engine is up it only sets the path the
// init will load. Takes the spine gate (<= 1 s); false = busy (a turn is running), nothing changed.
bool nucleo_anima_session_open(const char *ctx_path);

// On-device DEDUCTIVE tier (HDC/permutation-KGE): grow a knowledge graph over the learned triples
// (mind.<lang>.jsonl), detect a fact question (forward "quando e nato X" / inverse "capitale di X" /
// transitive "in che continente e X") and DEDUCE the answer by composing relation-rotations — answering
// facts never literally stored, gated by resonance coherence (refuses rather than fabricating). Returns
// true and fills *out (tier FACT, action ANSWER, intent "hdc") only above the gate; false = no answer.
// Additive: nucleo_anima_query() calls it on an offline-cascade miss, before the online tiers.
bool nucleo_anima_hdc_reason(const char *query, const char *lang, anima_result_t *out);

// Detect-only companion to the deductive tier: if `query` is a fact question, fill `rel` with the
// structured relation token ("born"|"died"|"capital"|"located_in"|"author") and `subj` with the
// extracted entity name, and return true — WITHOUT building the KG or reasoning. Lets the orchestrator
// capture the conversational focus from the QUERY structure even when another tier (e.g. an L1 card)
// produced the answer, so a bare follow-up ("e newton?") can re-aim the reasoner. False = not a fact Q.
bool nucleo_anima_hdc_detect(const char *query, char *rel, size_t rcap, char *subj, size_t scap);

// NEURO-SYMBOLIC COMBINATOR tier (mirrors tools/anima/combinator.mjs): COMPUTE an answer by COMPOSING
// >=2 learned facts that exist as NO single stored triple — "chi e nato prima A o B" (year compare),
// "quanti anni tra la nascita di A e B" (subtraction), "X era europeo" (nationality->continent),
// "A e B erano connazionali" (country equality). Pulls the facts from the SAME learned triple store
// (mind.<lang>.jsonl) the deductive tier reads; pure integer/string composition, no generation -> cannot
// hallucinate. Returns true (and fills *out: tier FACT, action ANSWER, intent "combinator", confidence =
// min of the source confidences) when the query is compositional — EITHER with the computed answer, OR,
// if a required fact is missing, with an honest "non ho i dati" refuse (still true, to STOP the cascade
// so no later tier fabricates a bio for it). Returns false ONLY when the pattern doesn't match (continue).
// Runs as the FIRST reasoning tier (before nucleo_anima_hdc_reason) so a composition isn't mis-parsed.
bool nucleo_anima_combinator(const char *query, const char *lang, anima_result_t *out);

// NSPCG — NEURO-SYMBOLIC PROOF-CARRYING GENERATION (mirrors tools/anima/pcg.mjs). The generative leap:
// for a CLOSED explanation question ("perche X e in Y", "come e collegato X a Y") OR an OPEN locational
// one ("dove si trova X", "in che continente e X"), it DISCOVERS a mixed-relation derivation chain by
// itself over the learned triples (capital/located_in/country) — for the open form it even discovers the
// ENDPOINT (the most-general container) — VERBALIZES it into a sentence that exists NOWHERE in the corpus
// ("Einstein e di un paese che si trova in Europa, perche Einstein e di Germania e Germania si trova in
// Europa"), and only emits it if every hop both RESONATES (HDC coherence gate) and is a REAL stored edge
// (a self-checked proof tree) — hallucination-impossible by construction. Returns true and fills *out
// (tier FACT, intent "pcg") on a grounded, verified chain; false = not such a question OR no grounded
// chain (refuse). Additive: nucleo_anima_query() calls it on an offline miss, BEFORE hdc_reason (which
// stays the fall-through for "capitale di / quando e nato / in che paese" and any chain NSPCG refuses).
bool nucleo_anima_pcg_generate(const char *query, const char *lang, anima_result_t *out);

// Detect-only companion to NSPCG (parse, no KG build): true if `query` is an NSPCG-shaped question
// (where/why/bridge). Lets the orchestrator free L1's heap for the KG build ONLY when NSPCG will try.
bool nucleo_anima_pcg_detect(const char *query, const char *lang);

// TYPED-FACET tier (nucleo_anima_facet.c, mirrors docs/anima-knowledge-graph.md): answer a CATEGORICAL
// facet question — "che lavoro faceva X / di cosa si occupava X" (occupation), "X è uomo o donna / di che
// sesso è X" (gender) — by EXACT lookup in data/anima/learned/facets.<lang>.jsonl. Source-anchored, no
// generation -> cannot hallucinate; returns 0 (abstain) on an unknown entity or a wrong-type subject
// (a place has no occupation). Runs before the fuzzy L1/KGE tiers so a precise typed answer wins; `died`
// stays with the KGE (relational), categorical facets live here (the KGE's many-to-one fan-in won't cleanup).
int nucleo_anima_facet(const char *raw, bool en, anima_result_t *r);


#ifdef __cplusplus
}
#endif
