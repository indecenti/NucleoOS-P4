// NucleoOS — shared online-AI client for the shell surfaces (onboarding + copilot).
//
// The keys stay on the device. The vault (/data/anima/teacher.json, sealed to the chip) is read through
// /api/anima/teacher, which blanks every key: readTeacher() puts VAULT_KEY where a saved key is, so
// "is a key set?" checks keep working, and the device relay /api/llm adds the real key on the way out —
// only towards the host it was saved for. Every model call goes through that relay (it streams the
// answer back as it arrives). A key the user is typing (test before save, a model list) is the one key
// a page sends itself. Two wire formats: Anthropic (Claude — x-api-key + anthropic-version +
// /v1/messages + content[].text) and OpenAI-compatible (Groq/OpenAI/xAI/Gemini/LAN — Bearer +
// /chat/completions). Writes go to /api/anima/teacher too and are MERGED on the device: a blank key
// keeps the saved one, so a page can never wipe a key it was not shown.

export const AI_PATH = '/data/anima/teacher.json';
const TEACHER_API = '/api/anima/teacher';
// Where a saved key would be: never a secret, never sent (authHeaders), written back as "" (kept).
export const VAULT_KEY = '\u2022device-key\u2022';
export const isVaultKey = (k) => k === VAULT_KEY;
const realKey = (k) => (k && k !== VAULT_KEY ? k : '');

export const PROVIDERS = {
  anthropic: {
    label: 'Claude', base: 'https://api.anthropic.com', version: '2023-06-01',
    prefix: /^sk-ant-/, ph: 'sk-ant-…', def: 'claude-sonnet-5',
    // claude-sonnet-5: near-Opus quality at Sonnet price — the browser default. NOTE: unlike the 4.x
    // family it runs ADAPTIVE THINKING when the request omits `thinking`; anthBody() below disables
    // it explicitly (speed/cost — a chat UI answer, not a reasoning run). 4.6 kept for continuity.
    models: [['claude-sonnet-5', 'Sonnet 5 · consigliato'], ['claude-sonnet-4-6', 'Sonnet 4.6 · equilibrio'], ['claude-opus-4-8', 'Opus 4.8 · massima qualità'], ['claude-haiku-4-5', 'Haiku 4.5 · veloce/economico']],
  },
  openai: {
    label: 'Groq', base: 'https://api.groq.com/openai/v1', version: '',
    prefix: /^(gsk_|sk-)/, ph: 'gsk_…', def: 'llama-3.1-8b-instant',
    models: [['llama-3.1-8b-instant', 'Llama 3.1 8B · veloce'], ['llama-3.3-70b-versatile', 'Llama 3.3 70B · qualità']],
  },
  xai: {
    label: 'Grok (xAI)', base: 'https://api.x.ai/v1', version: '',
    prefix: /^xai-/, ph: 'xai-…', def: 'grok-2-latest',   // OpenAI-compatible wire (Bearer + /chat/completions)
    models: [['grok-2-latest', 'Grok 2 · latest'], ['grok-2-1212', 'Grok 2 (1212)'], ['grok-beta', 'Grok beta']],
  },
  google: {
    label: 'Gemini', base: 'https://generativelanguage.googleapis.com/v1beta/openai', version: '',
    prefix: /^(AIza|AQ\.)/, ph: 'AIza… / AQ.…', def: 'gemini-2.5-flash', proxy: true,   // proxy: CORS-blocked browser-direct → relay via /api/llm; keys: classic AIza… or newer AQ.… tokens
    // STATIC FALLBACK — the live calibrateGemini() refines this to the key's REAL /models. IDs verified
    // against the API: gemini-3.5-flash / gemini-3.1-pro do NOT exist (404). Real lineup: gemini-2.5-flash
    // (recommended, stable, free), gemini-flash-latest (always-current Flash), gemini-2.5-pro (paid quality),
    // gemini-2.5-flash-lite (cheap, weak — fine for quick lookups, NOT for careful code).
    models: [['gemini-2.5-flash', 'Gemini 2.5 Flash · consigliato'], ['gemini-flash-latest', 'Gemini Flash · ultimo'], ['gemini-2.5-pro', 'Gemini 2.5 Pro · qualità (a pagamento)'], ['gemini-2.5-flash-lite', 'Gemini 2.5 Flash-Lite · economico']],
  },
  // A model server on the LAN: Ollama (:11434), LM Studio (:1234), llama.cpp / LocalAI (:8080), any
  // OpenAI-compatible endpoint. No CORS and plain HTTP, so the browser reaches it through the device
  // relay (/api/llm); the key is optional and the models are whatever the server lists (listModels).
  local: {
    label: 'Server locale', base: 'http://192.168.1.10:11434/v1', version: '', local: true, keyOptional: true,
    prefix: /.*/, ph: '(facoltativa)', def: '', proxy: true, models: [],
    presets: [['Ollama', 11434], ['LM Studio', 1234], ['llama.cpp', 8080]],
  },
};

// What each provider can actually DO. The one source of truth for "Claude can't draw/transcribe",
// consumed by Settings' preset engine + the which-apps panel so a feature gap is shown, not hit silently.
// image → /images/generations (only xAI grok-2-image); whisper → audio transcription (Groq/OpenAI only);
// ir → the IR app's NL skill (apps/ir-remote line 607 accepts only Groq/Gemini today).
// ir = the IR app's NL skill now works on EVERY chat provider (cloudToolCall below: Anthropic tool_use +
// OpenAI-compat tool_calls), so it is true across the board — kept as a field so the matrix stays the
// single place that answers "which provider can do what".
export const CAPMATRIX = {
  anthropic: { chat: true, image: false, whisper: false, toolUse: true, ir: true },
  openai:    { chat: true, image: false, whisper: true,  toolUse: true, ir: true },   // Groq
  xai:       { chat: true, image: true,  whisper: false, toolUse: true, ir: true },
  google:    { chat: true, image: false, whisper: false, toolUse: true, ir: true },
  local:     { chat: true, image: false, whisper: false, toolUse: true, ir: true },
};
// Per-provider quality tiers, mapped to REAL ids from PROVIDERS.models (validated against the registry
// by the engine; google.max=Pro is offered only when geminiTier==='paid'). Single source so a preset
// can ask for "max"/"mid"/"fast" without re-hardcoding model strings.
export const TIERS = {
  anthropic: { max: 'claude-opus-4-8',          mid: 'claude-sonnet-5',      fast: 'claude-haiku-4-5' },
  openai:    { max: 'llama-3.3-70b-versatile',  mid: 'llama-3.1-8b-instant', fast: 'llama-3.1-8b-instant' },
  xai:       { max: 'grok-2-latest',            mid: 'grok-2-latest',        fast: 'grok-2-1212' },
  google:    { max: 'gemini-2.5-pro',           mid: 'gemini-2.5-flash',     fast: 'gemini-2.5-flash-lite' },
};
// (local: no tiers - the server runs the one model the user picked)

// ── multi-model router ────────────────────────────────────────────────────────────────────────────
// Leverage EVERY configured model: route a subtask to the best available across ALL keys in teacher.json's
// keys{} map. Coarse cross-provider hints (not a benchmark): strength drives 'hard' picks (deep reasoning /
// codegen), cost/speed drives 'fast' picks (planning, triage, file summaries). Capability needs (whisper,
// image) are answered by CAPMATRIX. Models come from TIERS so there is no second source of truth.
// Provider-level routing hints: `cost` orders 'fast' picks (cheapest/quickest first), `strength` orders
// 'mid'/'hard' picks (most capable first). The MODEL within the chosen provider comes from its difficulty
// tier (fast→TIERS.fast, mid→TIERS.mid, hard→TIERS.max), so there is one source of truth for model ids.
export const ROUTE_RANK = {
  anthropic: { cost: 2, strength: 10 },
  openai:    { cost: 1, strength: 7  },  // Groq Llama
  xai:       { cost: 3, strength: 7  },
  google:    { cost: 1, strength: 8  },
  local:     { cost: 0, strength: 5  },   // free and private, usually a smaller model
};
const TIER_OF = { fast: 'fast', mid: 'mid', hard: 'max' };

// Build a concrete call cfg {provider, base, model, key, version, exec} for a provider + a chosen model.
function cfgFor(provider, entry, model, exec) {
  const p = providerOf(provider);
  entry = entry || {};
  return { provider, base: entry.base || p.base, model: model || entry.model || p.def, key: entry.key || '', version: entry.version || p.version, exec: exec || entry.exec || 'browser' };
}

// Pick the best call cfg for a subtask across all configured keys.
//   spec   = { difficulty:'fast'|'mid'|'hard', capability?:'chat'|'whisper'|'image', exclude?:[provider] }
//   keys   = teacher.json keys{} map (provider -> {base,model,key,version}); only entries WITH a key count
//   active = the user's current cfg (readTeacher result); used for geminiTier + as the safe default
// Returns a cfg, or null when no configured key can serve the need (caller decides: degrade / decline honestly).
export function routeFor(spec = {}, keys = {}, active = null) {
  const difficulty = spec.difficulty === 'hard' ? 'hard' : spec.difficulty === 'mid' ? 'mid' : 'fast';
  const tier = TIER_OF[difficulty];
  const cap = spec.capability && spec.capability !== 'chat' ? spec.capability : null;
  const exclude = new Set(spec.exclude || []);
  const activeP = active && active.provider;
  keys = keys || {};
  const usable = (p) => keys[p] && (keys[p].key || (PROVIDERS[p] && PROVIDERS[p].keyOptional && keys[p].base && keys[p].model));
  const configured = Object.keys(keys).filter((p) => usable(p) && PROVIDERS[p] && !exclude.has(p));
  // Rank a provider set: cheapest for 'fast', strongest for 'mid'/'hard'; tie → the user's active provider.
  const rank = (list) => list.map((p) => ({ p, ...(ROUTE_RANK[p] || ROUTE_RANK.anthropic) }))
    .sort((a, b) => { const d = difficulty === 'fast' ? (a.cost - b.cost) : (b.strength - a.strength); return d !== 0 ? d : (a.p === activeP ? -1 : b.p === activeP ? 1 : 0); });
  // Gemini Pro (the 'max' tier) is PAID-only; without a detected paid plan fall back to Flash. The tier is
  // read from the CHOSEN provider's OWN key entry (buildTeacherDoc persists it per-key so it survives a
  // provider switch), then from `active` only when that provider is the active one.
  const modelFor = (p) => {
    let m = (TIERS[p] && TIERS[p][tier]) || (keys[p] && keys[p].model);
    const paid = (keys.google && keys.google.geminiTier === 'paid') || (active && active.provider === 'google' && active.geminiTier === 'paid');
    if (p === 'google' && tier === 'max' && !paid) m = TIERS.google.mid;
    return m;
  };
  // CAPABILITY routing (whisper/image): only a provider whose CAPMATRIX has it — pick the BEST such one.
  if (cap) {
    const able = rank(configured.filter((p) => CAPMATRIX[p] && CAPMATRIX[p][cap]));
    if (!able.length) return null;                       // honest: no configured key can do it
    return cfgFor(able[0].p, keys[able[0].p], modelFor(able[0].p));
  }
  // No configured key (or all excluded) → the active default, but never an EXCLUDED provider (exclude contract).
  if (!configured.length) return (active && active.key && !exclude.has(active.provider)) ? Object.assign({ exec: 'browser' }, active) : null;
  const chosen = rank(configured)[0];
  return cfgFor(chosen.p, keys[chosen.p], modelFor(chosen.p));
}

export const providerOf = (p) => PROVIDERS[p] || PROVIDERS.anthropic;
// Every provider goes through the device relay: the page holds no key, the device adds it (and CORS
// never decides what works). `provider` is kept for callers; the relay picks the key by host.
export const viaProxy = (provider, url) => '/api/llm?url=' + encodeURIComponent(url);
// A private / loopback / .local host — a server on the LAN (mirrors the firmware's url_is_local).
export const isLanUrl = (u) => {
  const m = /^https?:\/\/([^/:]+)/i.exec(String(u || '')); if (!m) return false;
  const h = m[1].toLowerCase();
  if (h === 'localhost' || h.endsWith('.local')) return true;
  const o = h.split('.').map(Number);
  return o.length === 4 && o.every((x) => x >= 0 && x <= 255) &&
    (o[0] === 10 || o[0] === 127 || (o[0] === 192 && o[1] === 168) || (o[0] === 172 && o[1] >= 16 && o[1] <= 31) || (o[0] === 169 && o[1] === 254));
};
// Only a key the user is typing goes out from the page; a saved one (VAULT_KEY) is added by the relay.
export const authHeaders = (cfg) => {
  const k = realKey(cfg && cfg.key);
  if (cfg && cfg.provider === 'anthropic') return Object.assign({ 'anthropic-version': cfg.version || '2023-06-01' }, k ? { 'x-api-key': k } : {});
  return k ? { authorization: 'Bearer ' + k } : {};
};

// The models a provider offers right now (GET <base>/models: OpenAI-compatible {data:[{id}]}, Ollama's
// {models:[{name}]}, Anthropic's list). Through the device relay for every provider, so CORS never
// decides what works. [[id, label]] sorted, or throws Error with the provider's message.
export async function listModels(cfg) {
  const p = providerOf(cfg.provider);
  const base = String(cfg.base || p.base || '').replace(/\/+$/, '');
  if (!base) throw new Error('base URL mancante');
  const path = cfg.provider === 'anthropic' ? '/v1/models' : '/models';
  const r = await fetch('/api/llm?url=' + encodeURIComponent(base + path), { headers: authHeaders(cfg), cache: 'no-store' });
  const j = await r.json().catch(() => null);
  if (!r.ok) throw new Error((j && j.error && (j.error.message || j.error)) || ('HTTP ' + r.status));
  const data = (j && (j.data || j.models)) || [];
  const ids = [...new Set(data.map((m) => String((m && (m.id || m.name || m.model)) || '').replace(/^models\//, '')).filter(Boolean))];
  const chatty = cfg.provider === 'google' ? ids.filter((id) => /^gemini/i.test(id) && !/embedding|image|tts|aqa|live/i.test(id))
               : ids.filter((id) => !/embed|whisper|tts|moderation|dall-e|image|guard/i.test(id));
  return chatty.sort().map((id) => [id, id]);
}

// ── Gemini plan/tier calibration ────────────────────────────────────────────────────────────────────
// Google doesn't expose "your plan" via the API, but it's INFERABLE: Pro models (gemini-*-pro) are PAID-ONLY
// (since 2026-04 the free tier serves Flash/Flash-Lite only). So a Pro-model call that SUCCEEDS ⟹ billing is on
// ⟹ "paid/Pro"; a Pro-model call REFUSED (4xx: quota/permission/not-found) ⟹ free tier. We also LIST the key's
// real models so the dropdown reflects what it can actually use (no stale hardcoded guesses). All through the
// /api/llm proxy (Gemini has no CORS) and BEST-EFFORT: every step degrades to a safe default, never throws.
const GEMINI_BASE = PROVIDERS.google.base;
// Recommend a STRONG flash (never -lite): 2.5-flash (stable, = the registry default) → flash-latest (current) → 3-flash → any non-lite flash.
const pickFlash = (ids) => ids.find((m) => /gemini-2\.5-flash$/.test(m)) || ids.find((m) => /gemini-flash-latest$/.test(m))
  || ids.find((m) => /gemini-3-flash/.test(m)) || ids.find((m) => /flash$/.test(m) && !/lite$/.test(m))
  || ids.find((m) => /flash/.test(m) && !/lite/.test(m)) || ids[0] || null;
const pickPro = (ids) => ids.find((m) => /gemini-2\.5-pro$/.test(m)) || ids.find((m) => /gemini-3\.1-pro$/.test(m))
  || ids.find((m) => /pro-latest$/.test(m)) || ids.find((m) => /-pro$/.test(m)) || null;

// GET the key's available models through the proxy → bare id list (['gemini-3.5-flash', …]) or null on failure.
export async function geminiListModels(cfg) {
  try {
    const url = '/api/llm?url=' + encodeURIComponent((cfg.base || GEMINI_BASE).replace(/\/+$/, '') + '/models');
    const r = await fetch(url, { headers: authHeaders({ provider: 'google', key: cfg.key }), cache: 'no-store' });
    if (!r.ok) return null;
    const j = await r.json().catch(() => null);
    const data = (j && (j.data || j.models)) || null;        // OpenAI-compat {data:[{id}]} | native {models:[{name}]}
    if (!Array.isArray(data)) return null;
    const ids = [...new Set(data.map((m) => String((m && (m.id || m.name)) || '').replace(/^models\//, ''))
      .filter((id) => /^gemini/i.test(id) && !/embedding|image|tts|aqa|live/i.test(id)))];   // chat models only
    return ids.length ? ids : null;
  } catch { return null; }
}

// One tiny call to a Pro model → tier. 200 ⟹ 'paid'; 4xx (quota/permission/not-found) ⟹ 'free'; else 'unknown'.
export async function geminiProbeTier(cfg, proModel = 'gemini-2.5-pro') {   // a REAL Pro model (gemini-3.1-pro 404s → would always read "free")
  try {
    const url = '/api/llm?url=' + encodeURIComponent((cfg.base || GEMINI_BASE).replace(/\/+$/, '') + '/chat/completions');
    const r = await fetch(url, { method: 'POST',
      headers: { 'content-type': 'application/json', ...authHeaders({ provider: 'google', key: cfg.key }) },
      body: JSON.stringify({ model: proModel, max_tokens: 1, messages: [{ role: 'user', content: 'hi' }] }) });
    if (r.ok) return 'paid';
    if (r.status >= 400 && r.status < 500) return 'free';    // Pro refused → no billing → free tier
    return 'unknown';                                        // 5xx / proxy-busy → can't tell
  } catch { return 'unknown'; }
}

// Best-effort calibration: the key's real models + billing tier + a tier-appropriate recommended model. Probes
// SEQUENTIALLY (one device TLS at a time, RAM-friendly). Never throws; falls back to the static registry list.
export async function calibrateGemini(cfg) {
  const fallback = PROVIDERS.google.models.map((m) => m[0]);
  const live = await geminiListModels(cfg);
  const models = live || fallback;
  const pro = pickPro(models);
  const flash = pickFlash(models) || models[0];
  let tier;
  if (pro) tier = await geminiProbeTier(cfg, pro);           // can it actually USE a Pro model? 200=paid, 4xx=free
  else if (live) tier = 'free';                              // listed real models, none Pro → free-tier key
  else tier = await geminiProbeTier(cfg, pickPro(fallback) || 'gemini-2.5-pro');   // no list → probe a real Pro model
  const proPick = pro || pickPro(fallback);
  const recommended = (tier === 'paid' && proPick) ? proPick : flash;   // paid → best Pro; free/unknown → free-safe Flash
  return { tier, models, recommended, hasPro: tier === 'paid' };
}

// Honest one-line tier label for the UI (bilingual).
export function geminiTierLabel(tier, en) {
  if (tier === 'paid') return en ? 'Paid plan · Pro models available' : 'Piano a pagamento · modelli Pro disponibili';
  if (tier === 'free') return en ? 'Free tier · Flash only (Pro needs billing)' : 'Free tier · solo Flash (Pro richiede billing)';
  return en ? 'Plan not detected' : 'Piano non rilevato';
}
// A saved key shows as the device's hint ("gsk_…wxyz"); a key being typed as its own ends.
export const maskKey = (k, hint) => (isVaultKey(k) ? (hint || '••••') : (k && k.length > 10 ? k.slice(0, 6) + '…' + k.slice(-4) : (k ? '…' : '')));

// Lightweight capability probe — preferred over readTeacher() to answer "is AI configured?" because
// it never returns the raw key. {hasKey, online, enabled, provider?, model?} or null on error.
export async function caps() {
  try { const r = await fetch('/api/anima/caps', { cache: 'no-store' }); return r.ok ? await r.json() : null; }
  catch { return null; }
}

// Short-TTL memo for teacher.json: it's read by ~10 modules (settings reads it up to 3x on open,
// chat/spreadsheet/ir-remote/games/paint/agent/dictation on first action) and changes only when the
// user edits a key. A 30 s cache collapses the open-burst of /api/fs/read into one device read;
// writeTeacher() and a fs.changed on the path invalidate it immediately, so a key change is never stale.
let _teacherCache = null, _teacherAt = 0;
const TEACHER_TTL = 30000;
export function invalidateTeacher() { _teacherCache = null; _teacherAt = 0; }

// The device's public view -> what the page works with: a saved key becomes VAULT_KEY (+ keyHint).
function fromPublic(j) {
  const sec = (o, name) => (o && o[name + '_set'] ? VAULT_KEY : ((o && typeof o[name] === 'string') ? o[name] : ''));
  const keys = {};
  for (const [p, e] of Object.entries((j && j.keys) || {})) {
    if (!e || typeof e !== 'object') continue;
    const c = Object.assign({}, e, { key: sec(e, 'key') });
    if (e.key_hint) c.keyHint = e.key_hint;
    delete c.key_set; delete c.key_hint;
    keys[p] = c;
  }
  return Object.assign({}, j, { key: sec(j, 'key'), keyHint: j.key_hint || '', vision_key: sec(j, 'vision_key'), keys });
}

// Read the active teacher config (paired). Returns a normalized cfg, {unpaired:true}, or null. Keys
// come back as VAULT_KEY (the device keeps them); an older firmware without /api/anima/teacher still
// answers through /api/fs/read.
export async function readTeacher(opts = {}) {
  if (!opts.fresh && _teacherCache && (Date.now() - _teacherAt) < TEACHER_TTL) return _teacherCache;
  try {
    let r = await fetch(TEACHER_API, { cache: 'no-store' });
    let pub = true;
    if (r.status === 404) { r = await fetch('/api/fs/read?path=' + encodeURIComponent(AI_PATH), { cache: 'no-store' }); pub = false; }
    if (r.status === 401 || r.status === 403) return { unpaired: true };   // transient — not cached
    if (!r.ok) return null;
    let j = JSON.parse(await r.text()) || {};
    if (pub) j = fromPublic(j);
    const provider = (j.provider && PROVIDERS[j.provider]) ? j.provider : (isLanUrl(j.base) ? 'local' : (j.base && /anthropic/.test(j.base) ? 'anthropic' : (j.base && /generativelanguage/.test(j.base) ? 'google' : (j.base && /x\.ai/.test(j.base) ? 'xai' : (j.key ? 'openai' : 'anthropic')))));
    const p = providerOf(provider);
    const cfg = { provider, base: j.base || p.base, model: j.model || p.def, key: j.key || '', keyHint: j.keyHint || '', version: j.version || p.version, exec: j.exec || 'browser', keys: j.keys || {}, geminiTier: j.geminiTier || '', stt_url: j.stt_url || '', stt_model: j.stt_model || '', vision_model: j.vision_model || '', vision_base: j.vision_base || '', vision_key: j.vision_key || '', vision: typeof j.vision === 'boolean' ? j.vision : null };
    _teacherCache = cfg; _teacherAt = Date.now();
    return cfg;
  } catch { return null; }
}

// Canonical teacher.json: the active provider at top-level (what the firmware reads) + a keys map
// so switching providers doesn't lose the other key.
export function buildTeacherDoc(cfg) {
  const p = providerOf(cfg.provider);
  // The detected Gemini plan lives BOTH on the per-key entry (so the multi-model router can read it after the
  // user switches the active provider away from Google) AND at the top level (what the firmware reads today).
  const entry = Object.assign({ base: cfg.base || p.base, model: cfg.model || p.def, key: cfg.key || '' },
    cfg.provider === 'anthropic' ? { version: cfg.version || p.version } : {},
    (cfg.provider === 'google' && cfg.geminiTier) ? { geminiTier: cfg.geminiTier } : {});
  const keys = Object.assign({}, cfg.keys || {});
  const keyless = providerOf(cfg.provider).keyOptional && entry.base && entry.model;   // a LAN server needs no key
  if (entry.key || keyless) keys[cfg.provider] = entry; else delete keys[cfg.provider];
  const extra = (cfg.provider === 'google' && cfg.geminiTier) ? { geminiTier: cfg.geminiTier } : {};   // top-level mirror for the firmware
  // The home speech-to-text server (whisper.cpp / speaches) rides along so a key change never drops it.
  const stt = cfg.stt_url ? Object.assign({ stt_url: cfg.stt_url }, cfg.stt_model ? { stt_model: cfg.stt_model } : {}) : {};
  // The multimodal settings too: the vision helper (a model that sees images, for a text-only chat
  // model) and the "vision" override of the detected capability.
  const vis = {};
  for (const k of ['vision_model', 'vision_base', 'vision_key']) if (cfg[k]) vis[k] = cfg[k];
  if (typeof cfg.vision === 'boolean') vis.vision = cfg.vision;
  const doc = Object.assign({ provider: cfg.provider, exec: cfg.exec || 'browser' }, entry, extra, stt, vis, { keys });
  return toDevice(doc);
}

// What goes to the device: a saved key (VAULT_KEY) is sent as "" — the device keeps it — and the
// page-side hints never leave the page.
function toDevice(o) {
  if (Array.isArray(o)) return o.map(toDevice);
  if (!o || typeof o !== 'object') return o;
  const out = {};
  for (const [k, v] of Object.entries(o)) {
    if (k === 'keyHint' || /_(set|hint)$/.test(k)) continue;
    out[k] = isVaultKey(v) ? '' : toDevice(v);
  }
  return out;
}

// Write the vault (paired): merged on the device, keys left blank are kept. true | 'unpaired' | false.
export async function writeTeacher(cfg) {
  try {
    const body = JSON.stringify(buildTeacherDoc(cfg));
    let r = await fetch(TEACHER_API, { method: 'POST', body });
    if (r.status === 404) r = await fetch('/api/fs/write?path=' + encodeURIComponent(AI_PATH), { method: 'POST', body });   // older firmware
    if (r.ok) invalidateTeacher();   // our own write changed it — drop the memo so the next read is fresh
    return r.ok ? true : ((r.status === 401 || r.status === 403) ? 'unpaired' : false);
  } catch { return false; }
}

// ── keys typed inside an app (AI Chat, Dictation) ────────────────────────────────────────────────
const originOf = (u) => { try { return new URL(u).origin; } catch { return ''; } };
const providerForBase = (base) => (/\bx\.ai\b/.test(base) ? 'xai' : /anthropic\.com/.test(base) ? 'anthropic'
  : /generativelanguage/.test(base) ? 'google' : isLanUrl(base) ? 'local' : 'openai');

// Does the device hold a key for `base`'s host (the relay will add it)? false when unpaired / unknown.
export async function hasKeyFor(base, opts = {}) {
  const t = await readTeacher(opts).catch(() => null);
  if (!t || t.unpaired) return false;
  const o = originOf(base);
  if (t.key && originOf(t.base) === o) return true;
  return Object.values(t.keys || {}).some((e) => e && e.key && originOf(e.base) === o);
}

// Save a key the user typed in an app into the device vault, under the provider of `base`'s host; the
// active provider stays unless there is none yet. true | 'unpaired' | false.
export async function saveAppKey(base, key, model) {
  const t = await readTeacher({ fresh: true }).catch(() => null);
  if (!t) return false;
  if (t.unpaired) return 'unpaired';
  const id = providerForBase(base);
  const b = String(base || providerOf(id).base).replace(/\/+$/, '');
  const keys = Object.assign({}, t.keys || {});
  keys[id] = Object.assign({}, keys[id] || {}, { base: b, model: model || (keys[id] && keys[id].model) || providerOf(id).def, key });
  const cfg = Object.assign({}, t, { keys });
  if (!t.key) Object.assign(cfg, { provider: id, base: b, model: keys[id].model, key, version: providerOf(id).version });
  return writeTeacher(cfg);
}

// Older pages kept a Groq key in this browser (localStorage 'groq.key', shared by AI Chat, Dictation and
// Recorder — Dictation even copied the device's key there). The device holds keys now: move it into the
// vault when the vault has none for that host, then drop it from the browser.
export async function adoptBrowserKey() {
  let k = '', base = '';
  try { k = localStorage.getItem('groq.key') || ''; base = localStorage.getItem('groq.base') || ''; } catch { return; }
  if (!k) return;
  base = (base || PROVIDERS.openai.base).replace(/\/+$/, '');
  if (!(await hasKeyFor(base, { fresh: true }))) {
    const r = await saveAppKey(base, k);
    if (r !== true) return;   // unpaired / offline: keep it until the vault can take it
  }
  try { localStorage.removeItem('groq.key'); } catch {}
}

// Sonnet 5 runs ADAPTIVE thinking when `thinking` is omitted (the 4.x family ran thinking-off):
// for these chat surfaces that's slower + costlier with no visible upside, and a tiny max_tokens
// (cloudPing uses 16) can be eaten by thinking before any text. Disable it explicitly — ONLY for
// sonnet-5* (fable-5 rejects "disabled" with a 400; 4.x is already off-by-default).
// EXPORTED: the ONE copy of this model-policy rule for every web surface — agent runtime and the
// games coach import it; when the rule changes (new family, new default) it changes HERE only.
export const anthThinking = (model) => (String(model || '').includes('sonnet-5') ? { thinking: { type: 'disabled' } } : {});

// One completion through the device relay. Returns the assistant text; throws Error on failure.
// opts.signal — an AbortSignal so a user-facing Stop can cancel a hung cloud call (copilot.js).
export async function cloudComplete(cfg, system, user, maxTokens, opts = {}) {
  if (cfg.provider === 'anthropic') {
    const model = cfg.model || PROVIDERS.anthropic.def;
    const resp = await fetch(viaProxy('anthropic', (cfg.base || PROVIDERS.anthropic.base).replace(/\/+$/, '') + '/v1/messages'), {
      method: 'POST', signal: opts.signal,
      headers: { 'content-type': 'application/json', ...authHeaders(cfg) },
      body: JSON.stringify({ model, max_tokens: maxTokens || 1024, ...anthThinking(model), ...(system ? { system } : {}), messages: [{ role: 'user', content: user }] }),
    });
    const j = await resp.json().catch(() => null);
    if (!resp.ok || !j || j.type === 'error') throw new Error((j && j.error && j.error.message) || ('HTTP ' + resp.status));
    return Array.isArray(j.content) ? j.content.filter((b) => b && b.type === 'text').map((b) => b.text).join('') : '';
  }
  const epurl = (cfg.base || PROVIDERS.openai.base).replace(/\/+$/, '') + '/chat/completions';
  const resp = await fetch(viaProxy(cfg.provider, epurl), {
    method: 'POST', signal: opts.signal,
    headers: { 'content-type': 'application/json', ...authHeaders(cfg) },
    body: JSON.stringify({ model: cfg.model || PROVIDERS.openai.def, temperature: 0.4, messages: [...(system ? [{ role: 'system', content: system }] : []), { role: 'user', content: user }] }),
  });
  const j = await resp.json().catch(() => null);
  if (!resp.ok || !j || j.error) throw new Error((j && j.error && (j.error.message || j.error)) || ('HTTP ' + resp.status));
  return (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || '';
}

// Provider-aware single-shot TOOL selection. `tools` is an OpenAI-style tool list (anima-skill's
// actionsToOpenAITools); `messages` is an OpenAI-style [{role,content}] history+turn. Returns
// {tool, args} | {text} | null; throws on a definite HTTP/network failure (callers try/catch → miss).
// Anthropic uses native tool_use, the others the OpenAI tool wire; all through the device relay.
// This is the ONE place a NucleoOS app needs to call to get tool-calling on EVERY provider —
// it replaces each app re-implementing the per-provider wire (the fragmentation we're undoing).
export async function cloudToolCall(cfg, { system, messages = [], tools = [], signal, maxTokens = 512, temperature = 0.2, responseFormat = null } = {}) {
  if (cfg.provider === 'anthropic') {
    const atools = (tools || []).map((t) => ({ name: t.function.name, description: t.function.description, input_schema: t.function.parameters || { type: 'object', properties: {} } }));
    const model = cfg.model || PROVIDERS.anthropic.def;
    const resp = await fetch(viaProxy('anthropic', (cfg.base || PROVIDERS.anthropic.base).replace(/\/+$/, '') + '/v1/messages'), {
      method: 'POST', signal,
      headers: { 'content-type': 'application/json', ...authHeaders(cfg) },
      body: JSON.stringify({ model, max_tokens: maxTokens, ...anthThinking(model), ...(system ? { system } : {}), ...(atools.length ? { tools: atools } : {}), messages }),
    });
    const j = await resp.json().catch(() => null);
    if (!resp.ok || !j || j.type === 'error') throw new Error((j && j.error && j.error.message) || ('HTTP ' + resp.status));
    const blocks = Array.isArray(j.content) ? j.content : [];
    const tu = blocks.find((b) => b && b.type === 'tool_use');
    if (tu) return { tool: tu.name, args: tu.input || {} };
    const text = blocks.filter((b) => b && b.type === 'text').map((b) => b.text).join('');
    return text ? { text } : null;
  }
  const epurl = (cfg.base || PROVIDERS.openai.base).replace(/\/+$/, '') + '/chat/completions';
  const resp = await fetch(viaProxy(cfg.provider, epurl), {
    method: 'POST', signal,
    headers: { 'content-type': 'application/json', ...authHeaders(cfg) },
    body: JSON.stringify({ model: cfg.model || PROVIDERS.openai.def, max_tokens: maxTokens, temperature, messages: [...(system ? [{ role: 'system', content: system }] : []), ...messages], ...(tools.length ? { tools, tool_choice: 'auto' } : {}), ...(responseFormat ? { response_format: responseFormat } : {}) }),
  });
  const j = await resp.json().catch(() => null);
  if (!resp.ok || !j || j.error) throw new Error((j && j.error && (j.error.message || j.error)) || ('HTTP ' + resp.status));
  const msg = j.choices && j.choices[0] && j.choices[0].message;
  if (!msg) return null;
  const tc = msg.tool_calls && msg.tool_calls[0];
  if (tc && tc.function) { let a = {}; try { a = JSON.parse(tc.function.arguments || '{}'); } catch {} return { tool: tc.function.name, args: a }; }
  return msg.content ? { text: msg.content } : null;
}

// Verify a key works (a tiny call through the relay). true/false; throws on a definite rejection.
export async function cloudPing(cfg) {
  const t = await cloudComplete(cfg, null, 'Reply with exactly: ok', 16);
  return /\bok\b/i.test(t || '');
}
