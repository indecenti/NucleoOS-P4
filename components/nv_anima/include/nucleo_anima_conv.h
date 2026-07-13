// ANIMA conversations + user memory ("mini Claude" layer).
//
// Two persistent stores on SD, both owned by nucleo_anima_conv.c:
//   /sdcard/data/anima/conv/<id>.j   — one conversation, append-only JSONL {"r":"u"|"a","ts":..,"t":".."}
//   /sdcard/data/anima/conv/<id>.m   — its meta {"v":1,"title","created","updated","n","cut","sum"}
//   /sdcard/data/anima/memory.jsonl  — user memory facts {"ts":..,"t":".."} (the device CLAUDE.md)
//
// The conversation layer gives every surface (web copilot, Cardputer, future native) the same
// durable history with a Claude-style rolling COMPACTION: when a conversation outgrows the context
// budget, the oldest turns are summarized by the cloud teacher into meta.sum and meta.cut advances —
// requests then carry [memory + summary] as extra system context plus only the recent tail, so RAM
// and token cost stay flat no matter how long the chat runs.
//
// THREADING: all functions here are SD-I/O only and are called from ONE writer at a time by design —
// the web ANIMA worker (spine-locked) for chat/compaction, the serial httpd task for CRUD while the
// worker is idle. No internal locking.
#pragma once
#include <stdbool.h>
#include "nucleo_anima.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- conversations ---------------------------------------------------------
#define NV_CONV_ID_CAP   16    // "c" + 8 hex + suffix + NUL
#define NV_CONV_MAX      20    // stored conversations; creating past this prunes the oldest
#define NV_CONV_MSG_CAP  3000  // chars stored per message (longer input/replies are clipped)

// Create a new conversation (optional title, may be NULL). Fills id (NV_CONV_ID_CAP). 0 ok, <0 err.
int  nucleo_anima_conv_create(char *id, int idcap, const char *title);
// JSON list of conversations, newest-updated first: {"convs":[{"id","title","updated","n"},…]}.
// Returns body length, <0 on error.
int  nucleo_anima_conv_list_json(char *out, int cap);
// Append one message (role 'u' or 'a'). Sets the title from the first user message. 0 ok, <0 err.
int  nucleo_anima_conv_append(const char *id, char role, const char *text);
// Tail of a conversation as JSON (heap, caller frees): {"id","title","sum","msgs":[{"r","ts","t"},…]}.
// tail caps the returned messages (<=60). Returns length, <0 on error.
int  nucleo_anima_conv_msgs_json(const char *id, int tail, char **out_heap);
// Delete a conversation (both files). 0 ok.
int  nucleo_anima_conv_delete(const char *id);
// Set/replace the title. 0 ok.
int  nucleo_anima_conv_set_title(const char *id, const char *title);

// Build the persistent-context system block for a conversation: user memory + rolling summary,
// it/en labels, bounded. Returns length (0 = nothing to inject), <0 on error.
int  nucleo_anima_conv_ctx_block(const char *id, bool en, char *out, int cap);
// Rolling compaction: when enough un-summarized turns accumulated, fold the oldest into meta.sum
// via the cloud teacher (one bounded LLM call). Returns 1 compacted, 0 nothing to do, <0 failed.
int  nucleo_anima_conv_compact(const char *id, bool en);

// One conversational turn with full context (memory + summary + recent tail), provider cascade
// underneath: appends the user message and the assistant reply to the conversation, compacts first
// when due. `id` may be empty/NULL -> a new conversation is created and written back to id_out
// (NV_CONV_ID_CAP). Returns 1 answered, 0 honest miss (offline/no key), <0 store error.
int  nucleo_anima_conv_chat(const char *id, const char *input, bool en,
                            anima_result_t *out, char *id_out, int idcap);

// ---- user memory (device CLAUDE.md) ---------------------------------------
#define NV_MEM_FACT_CAP  240   // chars per fact
#define NV_MEM_MAX       48    // stored facts; adding past this drops the oldest

// Append one fact. 0 ok, <0 err.
int  nucleo_anima_mem_add(const char *fact);
// Delete by timestamp id. 0 ok, <0 not found/err.
int  nucleo_anima_mem_del(long ts);
// JSON list: {"facts":[{"ts":…,"t":"…"},…]} oldest→newest. Returns length, <0 err.
int  nucleo_anima_mem_list_json(char *out, int cap);
// Bounded system-prompt block ("MEMORIA UTENTE:\n- …"). Returns length (0 = no memory), <0 err.
int  nucleo_anima_mem_block(char *out, int cap, bool en);
// Detect a "ricordati che… / remember that…" turn: on match stores the fact and writes a short
// confirmation into reply. Also answers "cosa ricordi di me / what do you remember". Returns true
// when the turn was fully handled here (caller must NOT send it to the LLM).
bool nucleo_anima_mem_capture(const char *input, bool en, char *reply, int rcap);

// ---- exports from nucleo_anima_online.c used by/with this layer -----------
// (the turn-based nucleo_anima_online_chat_conv lives in the component-private nucleo_anima_online.h
//  — anima_turn_t is not part of the public surface)
// Thin public wrapper over the provider cascade one-shot (temp 0.3) — used by compaction.
int  nucleo_anima_teacher_complete(const char *sys, const char *user, char *out, int cap);

#ifdef __cplusplus
}
#endif
