# ANIMA: usage modes and fallback ladder

This document defines the contract between ANIMA's modes and what each turn actually uses.
The reference code is `nucleo_anima_route()` / `nucleo_anima_query()` in
`components/nv_anima/nucleo_anima.c`; the tests are the "FALLBACK LADDER" section of
`tests/host/unit/test_anima.cpp`.

## Principle

The mode the user picks is a **wish**: each turn works with what is available at that moment.
ANIMA never stops working. If a piece is missing it steps down a rung instead of stopping:

1. **Language model** (cloud or LAN): conversation, agent, code.
2. **Web**: Wikipedia, Wikidata, weather, news, exchange rates, holidays. Needs internet, not a model.
3. **Device**: L0 (commands and tools), math solver, L1 (knowledge on SD), HDC.
   Always works, even without a network.

## The three resources, checked on every turn

| Resource | True when |
|---|---|
| `network` | the device has an IP and the mode is not *offline* |
| `web` | `network`, and the mode is not *local* (in local mode nothing leaves the LAN) |
| `model` | a model is configured for the mode (in *local* only a LAN server), the device is online, and the first endpoint is not paused after an error |

The pauses after an error (circuit breaker in `nucleo_anima_online.c`) are:
- wrong key or model (400/401/403/404): 10 minutes;
- quota exhausted (429): 1 minute;
- server unreachable or 5xx: 15 seconds.

Editing `teacher.json` resets the pauses.

## The modes (`anima.net`)

| Mode | With a usable model | Without a model (missing, paused, or not responding) | Without a network |
|---|---|---|---|
| **offline** | — | device | device |
| **local** | device + LAN model as a last resort | device | device |
| **hybrid** (default) | device + web + model as a last resort | device + web | device |
| **llm (agent)** | the model drives the turn, **except what the device does exactly** | device + web, **commands included** | device, commands included |

`nucleo_anima_route()` returns the rung for the next turn (`anima_run_t`):

| `run` | Meaning |
|---|---|
| `ANIMA_RUN_DEVICE` | device only |
| `ANIMA_RUN_WEB` | device + web, no model |
| `ANIMA_RUN_LOCAL_LLM` | device + LAN model as a last resort |
| `ANIMA_RUN_HYBRID` | device + web + model as a last resort |
| `ANIMA_RUN_AGENT` | the model drives |

`degraded` is true when the mode is *llm* but the model is not usable.

## Device first, even in agent mode

What the device does exactly never goes to the model, not even when the model is there
(`l0_exact` and `facts_answer`, before the agent block in `nucleo_anima.c`):
- calculations and their follow-ups ("12+34", then "per 4" gives 184; the local model had answered 11.5);
- time, date, system status, "what can you do", "what is NucleoOS";
- opening and closing apps, timers, alarms, volume, brightness;
- the Wikidata facts in the packs ("quando è nato X").

These are instant answers, always right and always the same. What is left to the model is conversation and actions
that need interpreting: "scrivi uno e due in t.txt" (it works out what to write) and questions with an attached
image (only it sees the image). Without a model, the device's answer still carries `(offline)` or
`(no model)`.

## Fallback rules

1. **Don't wait for a model that isn't there.** If it is not configured, if the device is offline or if it is paused,
   the turn starts straight from the device rungs, without trying to connect.
2. **One attempt per turn.** If the model fails during the turn, the rest of the turn sees it as
   missing (`nucleo_anima_online_model_off`). Translation, Wikipedia verification and the teacher do not call it again.
3. **Commands work in agent mode too.** If the model does not respond, "apri la musica" or
   "alza il volume" are executed by L0: the user asked for them, and on the model side nothing was executed.
4. **The answer says where it comes from.** In *llm* mode a text answer given by the device is
   prefixed with `(no model)` if the network is there, or with `(offline)` if it is missing. Commands stay clean.
   The result's `degraded` field (and `"degraded"` in the web JSON) lets the interface show it.
5. **An "I don't know" explains why.** It reports the model's problem (invalid key, quota, server
   unreachable), or "no language model available".
6. **Same rules on every surface:**
   - the native app, Telegram, `/api/anima` and `/api/anima/query` go through `nucleo_anima_query`;
   - the web conversational chat (`/api/anima/chat`, `nucleo_anima_conv_chat`), if the model is missing or does not
     respond, goes through `nucleo_anima_query_no_model`: the device answers, any command is
     executed, and the turn stays in the conversation.

## Shape of the turn: who owns a request

Before any level answers, `turn_gate()` (in `nucleo_anima.c`) looks at the **shape** of a request
that asks to *do* something. A command word inside a sentence is no longer enough to get it executed.
The principle is certainty: a wrong or half-done action is worse than no action.

| Shape | Example | What happens |
|---|---|---|
| compound | "chiudi la musica e apri le note" | if L0 understands **every** part → a single **plan** executed in order; otherwise nothing is executed |
| deferred | "apri la musica tra 10 minuti", "…alle 18", "…domani" | never executed right away |
| conditional | "se domani piove ricordami…" | never executed right away |
| recurring | "ogni mattina alle 8 dimmi il meteo" | never executed right away |
| how-to | "come si alza il volume?" | explained and offered: a "sì" executes it, a "no" cancels it |
| negated | "non aprire la musica" | "OK, I won't do anything." ("Va bene, non faccio niente." in Italian) |
| complaint | "il volume è troppo alto", "lo schermo è troppo luminoso" | one correction step (−10 / +10) |

**What L0 cannot do with certainty goes to the model.** This applies to compounds that L0 cannot plan and to
deferred, conditional and recurring requests. In *hybrid* and *local* mode the model is used only if it is
usable, as a last resort, and answers with `ACT` lines that are validated. If the model is missing, the
answer honestly explains what works today (for example a reminder). If L0 can plan a compound,
the model is not asked.

These are never split:
- the text of a note or a reminder ("crea una nota con scritto apri la porta e chiudi la finestra");
- a list inside a reminder ("ricordami di comprare il pane e il latte");
- questions ("chi erano Romolo e Remo").

A courtesy form ("se puoi apri la musica") is a simple request. A pronoun that refers to another
part of the sentence ("alza il volume e poi abbassalo", "chiudi la musica e riaprila") is ambiguous inside a
plan: nothing is executed and the answer asks you to say it in full.

### Plans

A plan is a validated list of device actions (`anima_result_t.steps`, at most `ANIMA_PLAN_MAX` = 3):
- at most **one app** to open (the system keeps one open at a time). Two requests for the same
  app ("apri la galleria e mostrami le foto") count as one;
- at most **one** note or reminder (they share the composed content);
- no contradictions (opening and closing the same app);
- device actions only: apps, volume, brightness, closing the music, notes, reminders.

The same validator applies to L0 and to the model. A model that answers with several `ACT` lines produces the
same plan. Permissions (`permissions.json`) take the most restrictive step, and a single "procedo?" covers
the whole plan. An invalid plan proposed by the model is not executed.

Execution: `nv_anima_os_run` executes the main action (if it is a tool) and then each step, in the order
requested. The plan's app is opened last by the surface, like any other opening. The answer joins
the parts ("Alzo il volume, apro Note e abbasso la luminosità."), and the surfaces show the localized name
of every app mentioned (`nv_anima_pretty_reply`). The web JSON adds `"steps":[{"intent","arg"}…]`.

## Where it shows

- `GET /api/anima/net` returns `mode`, `run`, `label`, `label_en`, `network`, `web`, `model` and `degraded`.
- Native ANIMA app: `/mode` shows the "now: …" line with the effective rung.

## What does NOT change

- In *hybrid* mode the model stays the last resort, for questions only (`a_is_askable`): commands and data
  never reach a generator.
- Pending confirmations, attached images and clarifications are resolved before the rung is chosen.
