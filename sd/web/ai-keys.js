// NucleoOS — shared online-AI key manager (the ONE place the OS asks for / stores a provider key).
//
// Before this module the key UI was copy-pasted into three surfaces (Settings, the ANIMA web app, and
// partly groq-chat), each with its OWN provider table + teacher.json read/write + cloud test — and they
// had already DRIFTED (ANIMA lacked xAI, accepted AQ. Gemini tokens the others rejected; two divergent
// key tests). This component is the single owner of that UI, built entirely on shell/ai.js (the single
// owner of the LOGIC: PROVIDERS/CAPMATRIX/TIERS, teacher.json I/O, cloud calls). Mount it; never re-implement.
//
// Persistence is /data/anima/teacher.json via the paired /api/fs/* — the device-held vault the firmware
// reads. The active provider sits at top-level (what the firmware loads) plus a keys{} map so switching
// Claude↔Groq never loses the other key. The RELEASE system (tools/deploy.ps1, sd-sync.ps1, sd_deploy.py)
// treats that file as device-state and never overwrites it — a key on the SD always wins over the repo.
//
//   import { mountKeyManager } from '/ai-keys.js';
//   const km = mountKeyManager(containerEl, { lang:'it', variant:'full', onChange(cfg){ /* refresh chrome */ } });
//   // handle: km.reload(), km.getCfg(), km.setLang('en'), km.destroy()
//
// opts: variant 'full' (Settings: provider chips + capability hint + Gemini plan + exec toggle + model + key)
//       | 'compact' (ANIMA drawer: provider chips + model + key). onChange(cfg) fires after load + every
//       mutation so the host can refresh dependent UI (preset engine, capability cards). exec:false hides
//       the browser/device toggle (kept from the loaded value); tier:false hides the Gemini plan row.

import * as AI from './ai.js';
import * as GPU from './webllm.js';

const STR = {
  it: {
    checking: 'controllo…', saving: 'salvo…', testing: 'provo…', deleting: 'elimino…',
    active: 'Chiave attiva:', nokey: 'Nessuna chiave per questo provider.', noset: 'Nessuna chiave impostata.',
    typefirst: 'Scrivi prima una chiave.', notlooklike: 'Non sembra una chiave ', saveanyway: '. Salvo lo stesso?',
    saved: 'Salvata:', deleted: 'Chiave eliminata.', delActive: 'Chiave eliminata. Attivo: ',
    confirmDel: 'Elimino la chiave ', confirmDelTail: ' da questo dispositivo?',
    pair: 'Accoppia il browser per gestire la chiave.', pairSave: 'Accoppia prima il browser (pairing).',
    cantread: 'Non riesco a leggere la chiave.', savefail: 'Salvataggio fallito.', delfail: 'Eliminazione fallita.',
    works: '✓ La chiave funziona — ', rejected: '✗ Chiave rifiutata.', typeortest: 'Inserisci o salva prima una chiave.',
    key: 'Chiave', model: 'Modello', exec: 'Esecuzione', plan: 'Piano', detect: '🔎 Rileva piano',
    browser: 'Browser', device: 'Device', save: '💾 Salva', test: '⚡ Prova', del: '🗑 Elimina',
    note: '🔒 La chiave è salvata solo sul tuo dispositivo (SD), mai inviata altrove né nei log. In Browser resta nel tuo browser e va diretta al provider — il dispositivo non viene caricato.',
    execBrowser: 'Le superfici web chiamano il provider direttamente dal browser: il dispositivo non viene caricato.',
    execDevice: 'Il device effettua le chiamate online (TLS leggero, gestito dall’arbitro anti-OOM).',
    paid: 'a pagamento · Pro disponibile', free: 'Free tier · solo Flash', noplan: 'piano non rilevato',
    netmode: 'Modalità', base: 'Server', refresh: '↻ Elenco', listing: 'chiedo i modelli al server…',
    listed: (n) => n + ' modelli disponibili', nolist: 'Il server non ha restituito modelli', typemodel: 'nome del modello (es. llama3.2)',
    localok: (n, m) => '✓ Server raggiungibile — ' + n + ' modelli' + (m ? ' · prova ' + m + ' ok' : ''),
    needbase: 'Inserisci l’indirizzo del server (http://<pc>:<porta>/v1).', needmodel: 'Scegli o scrivi il modello.',
    localnote: 'Server locale: il dispositivo fa da ponte (il browser non può parlare direttamente con Ollama). La chiave serve solo se il server la richiede. Su Ollama, sul PC: OLLAMA_HOST=0.0.0.0 ollama serve.',
    nets: [['offline', 'Offline', 'Solo il dispositivo, niente rete'], ['local', 'Locale', 'Dispositivo + server LLM nella tua rete, niente internet'],
           ['hybrid', 'Ibrida', 'Dispositivo, poi Wikipedia, poi il modello'], ['llm', 'LLM', 'Prima il modello, il dispositivo come riserva']],
    ws: 'Workspace di ANIMA (stile OpenClaw)', wsfile: 'File', wssave: 'Salva', wsex: 'Esempio', wssaved: 'salvato', wsempty: '(vuoto: non usato)',
    wsdesc: { 'SOUL.md': 'Chi è ANIMA: tono, valori, limiti. Va nel prompt del modello a ogni risposta.', 'USER.md': 'Chi sei tu: nome, abitudini, preferenze. Va nel prompt del modello.', 'HEARTBEAT.md': 'La checklist dei controlli proattivi: ANIMA la rilegge ogni tanto e ti avvisa solo se serve.' },
    perm: 'Permessi delle azioni del modello', permlv: { allow: 'consenti', ask: 'chiedi', deny: 'nega' },
    permnames: { open_app: 'aprire app', close_app: 'fermare la musica', set_volume: 'volume', set_brightness: 'luminosità', add_event: 'promemoria/calendario', create_file: 'creare file' },
    permnote: '"chiedi": ANIMA propone l\'azione e aspetta il tuo sì. Vale per le azioni decise da un modello; i comandi che dai tu restano diretti.',
    hb: 'Controlli proattivi', hbev: [[0, 'spenti'], [15, 'ogni 15 min'], [30, 'ogni 30 min'], [60, 'ogni ora']], hbnext: (n) => n < 0 ? 'nessuna checklist (scrivi HEARTBEAT.md)' : `prossimo tra ${n} min`,
    wake: 'Voce a mani libere', wakeon: 'Ascolta la parola di attivazione', wakeword: 'Parola', wakesens: 'Sensibilità', sens: ['Bassa', 'Normale', 'Alta'],
    wakest: { off: 'spenta', listening: 'in ascolto', heard: 'sentita: ascolto la domanda', paused: 'in pausa', unavailable: 'non disponibile' },
    wakehint: (w) => `Di' «${w}», poi la domanda: ANIMA smette di ascoltare quando taci e risponde a voce. Tutto sul dispositivo, senza rete.`,
    waketrig: (n, s) => `${n} attivazioni` + (s >= 0 ? ` · ultima ${s < 60 ? s + ' s' : Math.round(s / 60) + ' min'} fa` : ''),
    wakestt: { home: 'La domanda viene trascritta dal server di casa ', cloud: 'La domanda viene trascritta nel cloud: ', none: 'Manca la trascrizione: imposta qui sotto un server Whisper di casa o una chiave Groq/OpenAI' },
    wakebuild: 'Questa build non include il riconoscimento: va attivata l\'opzione NV_WAKE_ESP_SR nel firmware (vedi components/nv_wake/Kconfig).',
    stt: 'Trascrizione voce in casa (Whisper)', sttph: 'http://192.168.1.20:8080/inference', sttsave: 'Salva', sttok: 'salvato: la voce viene trascritta prima da questo server', sttoff: 'nessun server: si usa la chiave cloud (se c\'è)', sttbad: 'deve essere un indirizzo della rete di casa (192.168.x.x, 10.x, .local)',
    sttnote: 'Un PC di casa trascrive la voce in 99 lingue senza cloud. whisper.cpp: whisper-server -m ggml-small.bin --host 0.0.0.0 --port 8080 (indirizzo …/inference); oppure speaches / LocalAI (…/v1/audio/transcriptions).',
    gpu: 'Modello nel browser (WebGPU)', gpuprobe: 'controllo la GPU…', gpuload: '⬇ Scarica e carica', gpuunload: '⏏ Libera GPU',
    gpudel: '🗑 Rimuovi dalla cache', gputest: '⚡ Prova', gpuuse: 'Usa nel Copilot quando il dispositivo non sa rispondere',
    gpunone: 'WebGPU non disponibile: ', gpucached: 'in cache', gpubig: 'troppo grande per questa GPU', gpuready: '✓ Pronto sulla GPU: ',
    gpunote: 'Gira sulla GPU di questo computer: niente cloud, niente chiavi. Il primo caricamento scarica i pesi (centinaia di MB, poi restano in cache).',
  },
  en: {
    checking: 'checking…', saving: 'saving…', testing: 'testing…', deleting: 'deleting…',
    active: 'Active key:', nokey: 'No key for this provider.', noset: 'No key set.',
    typefirst: 'Type a key first.', notlooklike: 'That does not look like a ', saveanyway: ' key. Save anyway?',
    saved: 'Saved:', deleted: 'Key deleted.', delActive: 'Key deleted. Active: ',
    confirmDel: 'Delete the ', confirmDelTail: ' key from this device?',
    pair: 'Pair this browser to manage the key.', pairSave: 'Pair this browser first (Settings ▸ pairing).',
    cantread: 'Could not read the key.', savefail: 'Save failed.', delfail: 'Delete failed.',
    works: '✓ Key works — ', rejected: '✗ Key rejected.', typeortest: 'Type or save a key first.',
    key: 'Key', model: 'Model', exec: 'Execution', plan: 'Plan', detect: '🔎 Detect plan',
    browser: 'Browser', device: 'Device', save: '💾 Save', test: '⚡ Test', del: '🗑 Delete',
    note: '🔒 The key is stored only on your device (SD), never sent elsewhere or logged. In Browser it stays in your browser and goes straight to the provider — the device isn’t loaded.',
    execBrowser: 'Web surfaces call the provider straight from the browser: the device isn’t loaded.',
    execDevice: 'The device makes the online calls (light TLS, handled by the anti-OOM arbiter).',
    paid: 'paid · Pro available', free: 'Free tier · Flash only', noplan: 'plan not detected',
    netmode: 'Mode', base: 'Server', refresh: '↻ List', listing: 'asking the server for its models…',
    listed: (n) => n + ' models available', nolist: 'The server returned no models', typemodel: 'model name (e.g. llama3.2)',
    localok: (n, m) => '✓ Server reachable — ' + n + ' models' + (m ? ' · ' + m + ' answered' : ''),
    needbase: 'Enter the server address (http://<pc>:<port>/v1).', needmodel: 'Pick or type the model.',
    localnote: 'Local server: the device bridges it (a browser cannot talk to Ollama directly). A key only if the server wants one. For Ollama, on the PC: OLLAMA_HOST=0.0.0.0 ollama serve.',
    nets: [['offline', 'Offline', 'The device only, no network'], ['local', 'Local', 'Device + an LLM server on your network, no internet'],
           ['hybrid', 'Hybrid', 'Device, then Wikipedia, then the model'], ['llm', 'LLM', 'The model first, the device as fallback']],
    ws: 'ANIMA workspace (OpenClaw-style)', wsfile: 'File', wssave: 'Save', wsex: 'Example', wssaved: 'saved', wsempty: '(empty: not used)',
    wsdesc: { 'SOUL.md': 'Who ANIMA is: tone, values, limits. Goes into the model prompt on every answer.', 'USER.md': 'Who you are: name, habits, preferences. Goes into the model prompt.', 'HEARTBEAT.md': 'The proactive checklist: ANIMA re-reads it now and then and notifies you only when needed.' },
    perm: 'Permissions for model actions', permlv: { allow: 'allow', ask: 'ask', deny: 'deny' },
    permnames: { open_app: 'open apps', close_app: 'stop music', set_volume: 'volume', set_brightness: 'brightness', add_event: 'reminders/calendar', create_file: 'create files' },
    permnote: '"ask": ANIMA proposes the action and waits for your yes. Applies to actions a model decides; your own commands stay direct.',
    hb: 'Proactive checks', hbev: [[0, 'off'], [15, 'every 15 min'], [30, 'every 30 min'], [60, 'hourly']], hbnext: (n) => n < 0 ? 'no checklist (write HEARTBEAT.md)' : `next in ${n} min`,
    wake: 'Hands-free voice', wakeon: 'Listen for the wake word', wakeword: 'Word', wakesens: 'Sensitivity', sens: ['Low', 'Normal', 'High'],
    wakest: { off: 'off', listening: 'listening', heard: 'heard: taking the question', paused: 'paused', unavailable: 'unavailable' },
    wakehint: (w) => `Say "${w}", then your question: ANIMA stops listening when you go quiet and answers aloud. All on the device, no network.`,
    waketrig: (n, s) => `${n} activations` + (s >= 0 ? ` · last ${s < 60 ? s + ' s' : Math.round(s / 60) + ' min'} ago` : ''),
    wakestt: { home: 'The question is transcribed by the home server ', cloud: 'The question is transcribed in the cloud: ', none: 'No transcription set: add a home Whisper server below or a Groq/OpenAI key' },
    wakebuild: 'This build has no detector: enable NV_WAKE_ESP_SR in the firmware (see components/nv_wake/Kconfig).',
    stt: 'Voice transcription at home (Whisper)', sttph: 'http://192.168.1.20:8080/inference', sttsave: 'Save', sttok: 'saved: voice is transcribed by this server first', sttoff: 'no server: the cloud key is used (if any)', sttbad: 'must be a home-network address (192.168.x.x, 10.x, .local)',
    sttnote: 'A home PC transcribes voice in 99 languages with no cloud. whisper.cpp: whisper-server -m ggml-small.bin --host 0.0.0.0 --port 8080 (address …/inference); or speaches / LocalAI (…/v1/audio/transcriptions).',
    gpu: 'Model in the browser (WebGPU)', gpuprobe: 'checking the GPU…', gpuload: '⬇ Download & load', gpuunload: '⏏ Free the GPU',
    gpudel: '🗑 Remove from cache', gputest: '⚡ Test', gpuuse: 'Use it in the Copilot when the device has no answer',
    gpunone: 'WebGPU not available: ', gpucached: 'cached', gpubig: 'too big for this GPU', gpuready: '✓ Ready on the GPU: ',
    gpunote: 'Runs on this computer’s GPU: no cloud, no keys. The first load downloads the weights (hundreds of MB, then cached).',
  },
};

// One short capability hint per provider (full variant), derived from CAPMATRIX so it never drifts.
function capHint(p, en) {
  const c = AI.CAPMATRIX[p] || {};
  const can = [], cant = [];
  (en ? [['chat', 'chat'], ['image', 'image gen'], ['whisper', 'voice transcription'], ['toolUse', 'OS tools']]
      : [['chat', 'chat'], ['image', 'immagini'], ['whisper', 'trascrizione voce'], ['toolUse', 'strumenti OS']])
    .forEach(([k, lbl]) => (c[k] ? can : cant).push(lbl));
  return (en ? 'Can: ' : 'Sa: ') + can.join(', ') + (cant.length ? (en ? ' · No: ' : ' · No: ') + cant.join(', ') : '');
}

let _cssInjected = false;
function injectCss() {
  if (_cssInjected) return;
  _cssInjected = true;
  const s = document.createElement('style');
  s.textContent = `
.nkm{display:flex;flex-direction:column;gap:10px;font:inherit}
.nkm-chips{display:flex;flex-wrap:wrap;gap:6px}
.nkm-chip{cursor:pointer;border:1px solid var(--line,#2a2a35);background:var(--panel,#1a1a22);color:var(--ink,var(--muted,#cfcfe0));
  border-radius:var(--r-pill,999px);padding:5px 11px;font-size:13px;line-height:1;transition:.12s}
.nkm-chip:hover{border-color:var(--accent,#9b8cff)}
.nkm-chip.cur{background:var(--accent,#9b8cff);color:var(--accent-on,var(--ink,#0e0e12));border-color:var(--accent,#9b8cff);font-weight:600}
.nkm-chip .ok{opacity:.7;margin-left:5px}
.nkm-row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.nkm-row label{min-width:64px;color:var(--dim,#8a8a9a);font-size:13px}
.nkm select,.nkm input{background:var(--field,var(--bg,#0e0e12));color:var(--ink,#e8e8ee);border:1px solid var(--line,#2a2a35);
  border-radius:var(--r-sm,8px);padding:7px 9px;font:inherit;font-size:14px}
.nkm input{flex:1;min-width:180px}
.nkm select{cursor:pointer;min-width:190px}
.nkm-seg{display:inline-flex;border:1px solid var(--line,#2a2a35);border-radius:var(--r-sm,8px);overflow:hidden}
.nkm-seg .it{cursor:pointer;padding:6px 12px;font-size:13px;color:var(--dim,#8a8a9a)}
.nkm-seg .it.on{background:var(--accent,#9b8cff);color:var(--accent-on,#0e0e12);font-weight:600}
.nkm-btns{display:flex;gap:7px;flex-wrap:wrap}
.nkm-btn{cursor:pointer;border:1px solid var(--line,#2a2a35);background:var(--panel,#1a1a22);color:var(--ink,#e8e8ee);
  border-radius:var(--r-sm,8px);padding:7px 12px;font:inherit;font-size:13px}
.nkm-btn:hover{border-color:var(--accent,#9b8cff)}
.nkm-btn.primary{background:var(--accent,#9b8cff);color:var(--accent-on,#0e0e12);border-color:var(--accent,#9b8cff);font-weight:600}
.nkm-btn.danger:hover{border-color:var(--bad,var(--danger-text,#ff6b6b));color:var(--bad,var(--danger-text,#ff6b6b))}
.nkm-cap,.nkm-stat,.nkm-exechint{font-size:12.5px;color:var(--dim,#8a8a9a);line-height:1.4}
.nkm-stat b{color:var(--ink,#e8e8ee)}
.nkm-note{font-size:12px;color:var(--dim,#8a8a9a);line-height:1.45;opacity:.85}
.nkm-badge{font-size:12px;padding:2px 8px;border-radius:var(--r-pill,999px);border:1px solid var(--line,#2a2a35);color:var(--dim,#8a8a9a)}
.nkm-sec{border-top:1px solid var(--line,#2a2a35);padding-top:10px;margin-top:4px;display:flex;flex-direction:column;gap:8px}
.nkm-sec h4{margin:0;font-size:13.5px;color:var(--ink,#e8e8ee)}
.nkm-bar{height:6px;border-radius:3px;background:var(--line,#2a2a35);overflow:hidden}
.nkm-bar i{display:block;height:100%;width:0;background:var(--accent,#9b8cff);transition:width .2s}
.nkm-out{font-size:12.5px;white-space:pre-wrap;color:var(--ink,#e8e8ee);background:var(--field,#0e0e12);border:1px solid var(--line,#2a2a35);border-radius:var(--r-sm,8px);padding:7px 9px;max-height:140px;overflow:auto}
.nkm-check{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--ink,#e8e8ee);cursor:pointer}
.nkm-ta{width:100%;box-sizing:border-box;min-height:120px;resize:vertical;font:13px/1.45 ui-monospace,Menlo,Consolas,monospace;background:var(--field,var(--bg,#0e0e12));color:var(--ink,#e8e8ee);border:1px solid var(--line,#2a2a35);border-radius:var(--r-sm,8px);padding:8px}
.nkm-perms{display:grid;max-width:440px;grid-template-columns:minmax(120px,1fr) auto;gap:6px 12px;align-items:center;font-size:13px}
.nkm-perms select{min-width:110px}
.nkm-check input{flex:0 0 auto;min-width:0;width:16px;height:16px;margin:0;padding:0}`;
  document.head.appendChild(s);
}

const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

export function mountKeyManager(container, opts = {}) {
  injectCss();
  let lang = opts.lang === 'en' ? 'en' : 'it';
  const variant = opts.variant === 'compact' ? 'compact' : 'full';
  const full = variant === 'full';
  const showExec = full && opts.exec !== false;
  const showTier = opts.tier !== false;
  const onChange = typeof opts.onChange === 'function' ? opts.onChange : () => {};
  const t = () => STR[lang];

  // The live config (one per mount). keys{} remembers every provider's saved config so a switch is lossless.
  let cfg = { provider: 'anthropic', base: AI.PROVIDERS.anthropic.base, model: AI.PROVIDERS.anthropic.def,
              key: '', version: '2023-06-01', exec: 'browser', keys: {}, geminiTier: '', geminiModels: null };
  let hydrated = false;
  const prov = () => AI.providerOf(cfg.provider);

  // ---- DOM skeleton ----
  const root = document.createElement('div');
  root.className = 'nkm';
  const providerIds = Object.keys(AI.PROVIDERS);
  root.innerHTML =
    (full ? `<div class="nkm-row"><label>${esc(t().netmode)}</label><span class="nkm-seg" data-el="net">${t().nets.map(([id, lbl]) => `<span class="it" data-n="${id}">${esc(lbl)}</span>`).join('')}</span></div><div class="nkm-exechint" data-el="nethint"></div>` : '') +
    `<div class="nkm-chips" data-el="chips"></div>` +
    (full ? `<div class="nkm-cap" data-el="cap"></div>` : '') +
    (full && showTier ? `<div class="nkm-row" data-el="tierrow" style="display:none"><label>${esc(t().plan)}</label><span class="nkm-badge" data-el="tier">…</span><button type="button" class="nkm-btn" data-el="detect">${esc(t().detect)}</button></div>` : '') +
    `<div class="nkm-row" data-el="baserow" style="display:none"><label>${esc(t().base)}</label><input data-el="base" type="url" autocomplete="off" spellcheck="false" placeholder="http://192.168.1.10:11434/v1"><span class="nkm-btns" data-el="presets"></span></div>` +
    `<div class="nkm-row"><label>${esc(t().model)}</label><select data-el="model"></select><input data-el="modeltxt" style="display:none" autocomplete="off" spellcheck="false" placeholder="${esc(t().typemodel)}"><button type="button" class="nkm-btn" data-el="refresh">${esc(t().refresh)}</button></div>` +
    `<div class="nkm-note" data-el="localnote" style="display:none">${esc(t().localnote)}</div>` +
    (showExec ? `<div class="nkm-row"><label>${esc(t().exec)}</label><span class="nkm-seg" data-el="exec"><span class="it" data-x="browser">${esc(t().browser)}</span><span class="it" data-x="device">${esc(t().device)}</span></span></div>` : '') +
    (showExec ? `<div class="nkm-exechint" data-el="exechint"></div>` : '') +
    `<div class="nkm-row"><label>${esc(t().key)}</label><input data-el="key" type="password" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="sk-ant-…"></div>` +
    `<div class="nkm-btns"><button type="button" class="nkm-btn primary" data-el="save">${esc(t().save)}</button><button type="button" class="nkm-btn" data-el="test">${esc(t().test)}</button><button type="button" class="nkm-btn danger" data-el="del">${esc(t().del)}</button></div>` +
    `<div class="nkm-stat" data-el="stat">…</div>` +
    (full ? `<div class="nkm-note">${esc(t().note)}</div>` : '') +
    (full ? `<div class="nkm-sec" data-el="ws"><h4>${esc(t().ws)}</h4>` +
      `<div class="nkm-row"><label>${esc(t().wsfile)}</label><span class="nkm-seg" data-el="wsfiles">${['SOUL.md', 'USER.md', 'HEARTBEAT.md'].map((f) => `<span class="it" data-f="${f}">${f}</span>`).join('')}</span></div>` +
      `<div class="nkm-note" data-el="wsdesc"></div>` +
      `<textarea data-el="wstext" rows="7" spellcheck="false" class="nkm-ta"></textarea>` +
      `<div class="nkm-btns"><button type="button" class="nkm-btn primary" data-el="wssave">${esc(t().wssave)}</button><button type="button" class="nkm-btn" data-el="wsex">${esc(t().wsex)}</button><span class="nkm-stat" data-el="wsstat"></span></div>` +
      `<div class="nkm-row"><label>${esc(t().hb)}</label><select data-el="hbevery">${t().hbev.map(([v, l]) => `<option value="${v}">${esc(l)}</option>`).join('')}</select><span class="nkm-stat" data-el="hbnext"></span></div>` +
      `<h4>${esc(t().perm)}</h4><div class="nkm-perms" data-el="perms"></div><div class="nkm-note">${esc(t().permnote)}</div></div>` : '') +
    (full ? `<div class="nkm-sec" data-el="wake"><h4>${esc(t().wake)}</h4>` +
      `<div class="nkm-stat" data-el="wakestat">…</div>` +
      `<label class="nkm-check"><input type="checkbox" data-el="wakeon"> ${esc(t().wakeon)}</label>` +
      `<div class="nkm-row" data-el="wakewordrow" style="display:none"><label>${esc(t().wakeword)}</label><select data-el="wakeword"></select></div>` +
      `<div class="nkm-row" data-el="wakesensrow" style="display:none"><label>${esc(t().wakesens)}</label><span class="nkm-seg" data-el="wakesens">${t().sens.map((l, i) => `<span class="it" data-s="${i}">${esc(l)}</span>`).join('')}</span></div>` +
      `<div class="nkm-note" data-el="wakenote"></div></div>` : '') +
    (full ? `<div class="nkm-sec"><h4>${esc(t().stt)}</h4><div class="nkm-row"><input data-el="stt" type="url" autocomplete="off" spellcheck="false" placeholder="${esc(t().sttph)}"><button type="button" class="nkm-btn" data-el="sttsave">${esc(t().sttsave)}</button></div>` +
      `<div class="nkm-stat" data-el="sttstat"></div><div class="nkm-note">${esc(t().sttnote)}</div></div>` : '') +
    (full ? `<div class="nkm-sec" data-el="gpu"><h4>${esc(t().gpu)}</h4>` +
      `<div class="nkm-stat" data-el="gpustat">${esc(t().gpuprobe)}</div>` +
      `<div class="nkm-row" data-el="gpurow" style="display:none"><label>${esc(t().model)}</label><select data-el="gpumodel"></select></div>` +
      `<div class="nkm-bar" data-el="gpubar" style="display:none"><i></i></div>` +
      `<div class="nkm-btns" data-el="gpubtns" style="display:none"><button type="button" class="nkm-btn primary" data-el="gpuload">${esc(t().gpuload)}</button><button type="button" class="nkm-btn" data-el="gputest">${esc(t().gputest)}</button><button type="button" class="nkm-btn" data-el="gpuunload">${esc(t().gpuunload)}</button><button type="button" class="nkm-btn danger" data-el="gpudel">${esc(t().gpudel)}</button></div>` +
      `<label class="nkm-check" data-el="gpuuserow" style="display:none"><input type="checkbox" data-el="gpuuse"> ${esc(t().gpuuse)}</label>` +
      `<div class="nkm-out" data-el="gpuout" style="display:none"></div>` +
      `<div class="nkm-note">${esc(t().gpunote)}</div></div>` : '');
  container.innerHTML = '';
  container.appendChild(root);
  const $ = (n) => root.querySelector(`[data-el="${n}"]`);

  const setStat = (html) => { $('stat').innerHTML = html; };
  const statText = () => (prov().local && cfg.base && cfg.model)
    ? `${t().active} <b>${esc(cfg.base)}</b> · ${esc(prov().label)} · ${esc(cfg.model)}`
    : cfg.key
    ? `${t().active} <b>${esc(AI.maskKey(cfg.key))}</b> · ${esc(prov().label)} · ${esc(cfg.model)}`
    : t().nokey;

  function fillChips() {
    $('chips').innerHTML = providerIds.map((id) => {
      const set = !!(cfg.keys && cfg.keys[id] && cfg.keys[id].key) || (id === cfg.provider && cfg.key);
      return `<button type="button" class="nkm-chip${id === cfg.provider ? ' cur' : ''}" data-p="${id}">${esc(AI.PROVIDERS[id].label)}${set ? '<span class="ok">✓</span>' : ''}</button>`;
    }).join('');
    $('chips').querySelectorAll('[data-p]').forEach((b) => b.addEventListener('click', () => chooseProvider(b.dataset.p)));
  }
  function fillModels() {
    const sel = $('model'); if (!sel) return;
    const live = cfg.liveModels && cfg.liveModels.provider === cfg.provider ? cfg.liveModels.list : null;
    const list = live && live.length ? live
      : (cfg.provider === 'google' && cfg.geminiModels && cfg.geminiModels.length) ? cfg.geminiModels : prov().models.slice();
    if (cfg.model && !list.some((m) => m[0] === cfg.model)) list.unshift([cfg.model, cfg.model]);   // keep the saved one visible
    // A local server with no list yet: a free-text model name instead of an empty dropdown.
    const typed = !list.length;
    if ($('modeltxt')) { $('modeltxt').style.display = typed ? '' : 'none'; if (typed) $('modeltxt').value = cfg.model || ''; }
    sel.style.display = typed ? 'none' : '';
    sel.innerHTML = '';
    for (const [v, lbl] of list) { const o = document.createElement('option'); o.value = v; o.textContent = lbl; sel.appendChild(o); }
    if (typed) return;                               // the typed name stays the model
    sel.value = (cfg.model && list.some((m) => m[0] === cfg.model)) ? cfg.model : (list[0] ? list[0][0] : prov().def);
    cfg.model = sel.value;
  }
  function paint() {
    const p = prov();
    fillChips();
    if ($('key')) $('key').placeholder = p.ph;
    if (full && $('cap')) $('cap').textContent = capHint(cfg.provider, lang === 'en');
    if (full && showTier && $('tierrow')) {
      $('tierrow').style.display = cfg.provider === 'google' ? '' : 'none';
      if ($('tier')) $('tier').textContent = AI.geminiTierLabel(cfg.geminiTier, lang === 'en');
    }
    if (showExec && $('exec')) {
      // a LAN server is always reached through the device relay: "browser / device" means nothing there
      $('exec').closest('.nkm-row').style.display = prov().local ? 'none' : '';
      if ($('exechint')) $('exechint').style.display = prov().local ? 'none' : '';
      $('exec').querySelectorAll('.it').forEach((b) => b.classList.toggle('on', b.dataset.x === (cfg.exec || 'browser')));
      if ($('exechint')) $('exechint').textContent = (cfg.exec === 'device') ? t().execDevice : t().execBrowser;
    }
    const local = !!p.local;
    if ($('baserow')) { $('baserow').style.display = local ? '' : 'none'; if (local && document.activeElement !== $('base')) $('base').value = cfg.base || ''; }
    if ($('localnote')) $('localnote').style.display = local ? '' : 'none';
    if ($('presets') && local && !$('presets').childElementCount) {
      $('presets').innerHTML = (p.presets || []).map(([n, port]) => `<button type="button" class="nkm-btn" data-port="${port}">${esc(n)}</button>`).join('');
      $('presets').querySelectorAll('[data-port]').forEach((b) => b.addEventListener('click', () => {
        const host = (/^https?:\/\/([^/:]+)/.exec($('base').value || cfg.base || '') || [])[1] || '192.168.1.10';
        $('base').value = 'http://' + host + ':' + b.dataset.port + '/v1'; cfg.base = $('base').value;
      }));
    }
    fillModels();
  }

  function adopt(p) {
    const d = AI.providerOf(p), st = (cfg.keys && cfg.keys[p]) || {};
    cfg.provider = p; cfg.base = st.base || d.base; cfg.model = st.model || d.def;
    cfg.version = st.version || d.version; cfg.key = st.key || ''; cfg.geminiModels = null;
  }
  function chooseProvider(p) {
    if (!AI.PROVIDERS[p]) return;
    adopt(p); paint(); setStat(statText()); if ($('key')) $('key').value = '';
    onChange(getCfg());
  }

  async function reload() {
    setStat(t().checking); if ($('key')) $('key').value = '';
    const c = await AI.readTeacher({ fresh: true });
    if (c && c.unpaired) { setStat(t().pair); return; }
    if (!c) { setStat(t().cantread); return; }
    hydrated = true;
    cfg.keys = (c.keys && typeof c.keys === 'object') ? c.keys : {};
    cfg.exec = c.exec || 'browser'; cfg.geminiTier = c.geminiTier || ''; cfg.geminiModels = null;
    cfg.provider = c.provider || 'anthropic';
    const p = prov();
    cfg.base = c.base || p.base; cfg.model = c.model || p.def; cfg.key = c.key || ''; cfg.version = c.version || p.version;
    cfg.stt_url = c.stt_url || ''; cfg.stt_model = c.stt_model || '';
    if ($('stt')) { $('stt').value = cfg.stt_url; $('sttstat').textContent = cfg.stt_url ? t().sttok : t().sttoff; }
    if (cfg.key) cfg.keys[cfg.provider] = Object.assign({ base: cfg.base, model: cfg.model, key: cfg.key }, cfg.provider === 'anthropic' ? { version: cfg.version } : {});
    paint(); setStat((cfg.key || (prov().local && cfg.base)) ? statText() : t().noset);
    onChange(getCfg());
  }

  async function calibrate() {
    if (cfg.provider !== 'google' || !cfg.key) return;
    try {
      const c = await AI.calibrateGemini({ base: cfg.base, key: cfg.key });
      cfg.geminiTier = c.tier; cfg.geminiModels = (c.models || []).map((id) => [id, id]);
      if (!c.models.includes(cfg.model)) cfg.model = c.recommended;
      paint();
      const lbl = c.tier === 'paid' ? t().paid : c.tier === 'free' ? t().free : t().noplan;
      setStat(statText() + ' · <b>' + esc(lbl) + '</b>');
      await AI.writeTeacher(cfg);   // persist the detected plan + recommended model
      onChange(getCfg());
    } catch {}
  }

  const pickedModel = () => (($('modeltxt') && $('modeltxt').style.display !== 'none') ? $('modeltxt').value.trim() : ($('model') ? $('model').value : '')) || cfg.model;
  async function save() {
    const v = $('key').value.trim(), p = prov();
    if (p.local) {
      cfg.base = ($('base').value || '').trim().replace(/\/+$/, '');
      if (!AI.isLanUrl(cfg.base)) { setStat(t().needbase); return; }
      cfg.model = pickedModel();
      if (!cfg.model) { setStat(t().needmodel); return; }
      if (v) cfg.key = v;
    } else {
      if (!v) { setStat(t().typefirst); return; }
      if (!p.prefix.test(v) && !confirm(t().notlooklike + p.label + t().saveanyway)) return;
      cfg.key = v; cfg.model = pickedModel();
    }
    setStat(t().saving);
    const r = await AI.writeTeacher(cfg);
    if (r === true) {
      cfg.keys = cfg.keys || {};
      cfg.keys[cfg.provider] = Object.assign({ base: cfg.base, model: cfg.model, key: cfg.key || '' }, cfg.provider === 'anthropic' ? { version: cfg.version } : {});
      $('key').value = '';
      setStat(`${t().saved} <b>${esc(cfg.key ? AI.maskKey(cfg.key) : cfg.base)}</b> · ${esc(p.label)} · ${esc(cfg.model)}`);
      fillChips();
      onChange(getCfg());
      if (cfg.provider === 'google') calibrate();
    } else if (r === 'unpaired') setStat(t().pairSave);
    else setStat(t().savefail);
  }

  async function refreshModels() {
    const p = prov();
    const c = { provider: cfg.provider, base: p.local ? ($('base').value || '').trim() : (cfg.base || p.base), key: $('key').value.trim() || cfg.key, version: cfg.version || p.version };
    if (p.local && !AI.isLanUrl(c.base)) { setStat(t().needbase); return null; }
    if (!p.local && !c.key) { setStat(t().typeortest); return null; }
    setStat(t().listing);
    try {
      const list = await AI.listModels(c);
      cfg.liveModels = { provider: cfg.provider, list };
      fillModels();
      setStat(list.length ? t().listed(list.length) : t().nolist);
      return list;
    } catch (e) { setStat('✗ ' + esc(String(e.message || e))); return null; }
  }

  async function test() {
    if (prov().local) {
      const list = await refreshModels();
      if (!list) return;
      const model = pickedModel();
      if (!model) { setStat(t().listed(list.length)); return; }
      try {
        const ok = await AI.cloudPing({ provider: 'local', base: ($('base').value || '').trim(), model, key: $('key').value.trim() || cfg.key });
        setStat(t().localok(list.length, ok ? model : ''));
      } catch (e) { setStat('✗ ' + esc(String(e.message || e))); }
      return;
    }
    const v = ($('key').value.trim()) || cfg.key;
    if (!v) { setStat(t().typeortest); return; }
    const model = pickedModel();
    setStat(t().testing);
    try {
      const ok = await AI.cloudPing({ provider: cfg.provider, base: cfg.base || prov().base, model, key: v, version: cfg.version || prov().version });
      setStat(ok ? (t().works + esc(prov().label) + ' · ' + esc(model)) : t().rejected);
    } catch (e) { setStat('✗ ' + esc(String(e.message || e))); }
  }

  async function del() {
    if (!confirm(t().confirmDel + prov().label + t().confirmDelTail)) return;
    setStat(t().deleting);
    const keys = Object.assign({}, cfg.keys || {}); delete keys[cfg.provider];
    const remaining = Object.keys(keys).filter((k) => keys[k] && keys[k].key);
    try {
      if (remaining.length) {
        const np = remaining[0], e = keys[np];
        cfg.keys = keys;
        cfg.provider = np; cfg.base = e.base; cfg.model = e.model; cfg.key = e.key; cfg.version = e.version || '2023-06-01';
        const r = await AI.writeTeacher(cfg);
        if (r === true) { paint(); if ($('key')) $('key').value = ''; setStat(t().delActive + esc(AI.PROVIDERS[np].label)); onChange(getCfg()); return; }
        if (r === 'unpaired') { setStat(t().pairSave); return; }
        setStat(t().delfail); return;
      }
      const r = await fetch('/api/fs/delete?path=' + encodeURIComponent(AI.AI_PATH), { method: 'POST' });
      AI.invalidateTeacher();
      if (r.ok || r.status === 404) { cfg.key = ''; cfg.keys = {}; if ($('key')) $('key').value = ''; paint(); setStat(t().deleted); onChange(getCfg()); return; }
      if (r.status === 401 || r.status === 403) { setStat(t().pairSave); return; }
      setStat(t().delfail);
    } catch { setStat(t().delfail); }
  }

  function getCfg() { return { provider: cfg.provider, base: cfg.base, model: cfg.model, key: cfg.key, version: cfg.version, exec: cfg.exec, geminiTier: cfg.geminiTier, hasKey: !!cfg.key, keys: cfg.keys }; }

  // ---- network mode (the device's: /api/anima/net, same as the native /mode) ----
  function paintNet(mode) {
    if (!$('net')) return;
    $('net').querySelectorAll('.it').forEach((b) => b.classList.toggle('on', b.dataset.n === mode));
    const d = t().nets.find((n) => n[0] === mode);
    if ($('nethint')) $('nethint').textContent = d ? d[2] : '';
  }
  async function loadNet() {
    try { const r = await fetch('/api/anima/net', { cache: 'no-store' }); if (r.ok) paintNet((await r.json()).mode); } catch {}
  }
  if ($('net')) $('net').querySelectorAll('.it').forEach((b) => b.addEventListener('click', async () => {
    paintNet(b.dataset.n);
    try { const r = await fetch('/api/anima/net', { method: 'POST', body: JSON.stringify({ mode: b.dataset.n }) }); if (r.ok) paintNet((await r.json()).mode); } catch {}
    onChange(getCfg());
  }));

  // ---- the browser GPU model (WebGPU / WebLLM) ----
  let gpuList = [];
  const gpuOut = (txt) => { $('gpuout').style.display = txt ? '' : 'none'; $('gpuout').textContent = txt || ''; };
  const gpuBar = (f) => { $('gpubar').style.display = f == null ? 'none' : ''; $('gpubar').firstChild.style.width = Math.round((f || 0) * 100) + '%'; };
  function gpuPaint() {
    const sel = $('gpumodel'), cur = GPU.chosen();
    sel.innerHTML = gpuList.map((m) => `<option value="${esc(m.id)}"${m.fits ? '' : ' disabled'}>${esc(m.label)} · ${m.vramMB ? (m.vramMB / 1024).toFixed(1) + ' GB' : '?'}${m.cached ? ' · ' + esc(t().gpucached) : ''}${m.fits ? '' : ' · ' + esc(t().gpubig)}</option>`).join('');
    const def = gpuList.find((m) => m.id === cur) || gpuList.find((m) => m.cached && m.fits) || gpuList.find((m) => m.fits && m.small) || gpuList.find((m) => m.fits);
    if (def) sel.value = def.id;
    $('gpuuse').checked = (() => { try { return localStorage.getItem('anima.useWebGPU') === '1'; } catch { return false; } })();
  }
  async function gpuInit() {
    const caps = await GPU.probe();
    if (!caps.webgpu) { $('gpustat').textContent = t().gpunone + caps.reason; return; }
    $('gpustat').textContent = 'GPU: ' + (caps.adapter || 'WebGPU') + (caps.vramMB ? ' · ~' + (caps.vramMB / 1024).toFixed(1) + ' GB' : '') + (GPU.ready() ? ' · ' + t().gpuready + GPU.loaded() : '');
    try { gpuList = await GPU.catalogue(); } catch (e) { $('gpustat').textContent += ' · ' + String(e.message || e); return; }
    ['gpurow', 'gpubtns', 'gpuuserow'].forEach((n) => { $(n).style.display = ''; });
    gpuPaint();
  }
  if (full && $('gpu')) {
    $('gpuload').addEventListener('click', async () => {
      const id = $('gpumodel').value; if (!id) return;
      gpuOut(''); gpuBar(0);
      try {
        await GPU.load(id, (p) => { gpuBar(p.progress); $('gpustat').textContent = p.text || ''; });
        gpuBar(null); $('gpustat').textContent = t().gpuready + id;
        gpuList = await GPU.catalogue(); gpuPaint();
      } catch (e) { gpuBar(null); $('gpustat').textContent = '✗ ' + String(e.message || e); }
    });
    $('gputest').addEventListener('click', async () => {
      gpuOut('…');
      try {
        if (!GPU.ready() || GPU.loaded() !== $('gpumodel').value) await GPU.load($('gpumodel').value, (p) => gpuBar(p.progress));
        gpuBar(null);
        const t0 = Date.now();
        const txt = await GPU.chat([{ role: 'user', content: lang === 'en' ? 'Say hello in one short sentence.' : 'Saluta in una breve frase.' }], { maxTokens: 40, onToken: (_, all) => gpuOut(all) });
        gpuOut(txt + '\n(' + ((Date.now() - t0) / 1000).toFixed(1) + ' s)');
      } catch (e) { gpuBar(null); gpuOut('✗ ' + String(e.message || e)); }
    });
    $('gpuunload').addEventListener('click', async () => { await GPU.unload(); gpuInit(); });
    $('gpudel').addEventListener('click', async () => {
      const id = $('gpumodel').value; if (!id || !confirm(t().gpudel + ' ' + id + '?')) return;
      try { await GPU.remove(id); } catch {}
      gpuList = await GPU.catalogue().catch(() => gpuList); gpuPaint();
    });
    $('gpumodel').addEventListener('change', () => GPU.choose($('gpumodel').value));
    $('gpuuse').addEventListener('change', () => { try { localStorage.setItem('anima.useWebGPU', $('gpuuse').checked ? '1' : '0'); } catch {} onChange(getCfg()); });
  }

  // ---- wire ----
  $('refresh').addEventListener('click', refreshModels);
  if ($('base')) $('base').addEventListener('change', () => { cfg.base = $('base').value.trim(); cfg.liveModels = null; });
  $('save').addEventListener('click', save);
  $('test').addEventListener('click', test);
  $('del').addEventListener('click', del);
  $('key').addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); save(); } });
  if ($('model')) $('model').addEventListener('change', () => { cfg.model = $('model').value; });
  if ($('detect')) $('detect').addEventListener('click', () => { setStat(t().checking); calibrate(); });
  if ($('exec')) $('exec').querySelectorAll('.it').forEach((b) => b.addEventListener('click', () => {
    cfg.exec = b.dataset.x === 'device' ? 'device' : 'browser'; paint();
    AI.writeTeacher(cfg).then(() => onChange(getCfg()));   // exec is a teacher.json field — persist immediately
  }));

  paint();
  reload();
  if ($('sttsave')) $('sttsave').addEventListener('click', async () => {
    const u = $('stt').value.trim();
    if (u && !AI.isLanUrl(u)) { $('sttstat').textContent = t().sttbad; return; }
    cfg.stt_url = u;
    const r = await AI.writeTeacher(cfg);
    $('sttstat').textContent = r === true ? (u ? t().sttok : t().sttoff) : (r === 'unpaired' ? t().pair : t().cantread);
  });
  // ---- the workspace (OpenClaw-style files on the SD), heartbeat interval, model permissions ----
  const WS_DIR = '/data/anima/';
  const WS_EX = {
    'SOUL.md': lang === 'en'
      ? '# Who you are\nYou are ANIMA: warm, direct, a bit witty. Short answers unless asked for detail.\nNever pretend to have done something you did not do. Ask before anything irreversible.'
      : '# Chi sei\nSei ANIMA: calda, diretta, con un filo di ironia. Risposte brevi, a meno che non ti chieda dettagli.\nNon fingere mai di aver fatto qualcosa che non hai fatto. Chiedi prima di qualsiasi cosa irreversibile.',
    'USER.md': lang === 'en'
      ? '# About me\nName: …\nCity: … (for weather)\nI like: …\nPlease: call me by name, use metric units.'
      : '# Su di me\nNome: …\nCittà: … (per il meteo)\nMi piace: …\nPer favore: chiamami per nome, usa il sistema metrico.',
    'HEARTBEAT.md': lang === 'en'
      ? '- Is there an event in the next 2 hours? Remind me what and when.\n- Is tomorrow morning busy? Tell me tonight after 20:00.\n- Anything I asked to be reminded of today?'
      : '- C\'è un impegno nelle prossime 2 ore? Ricordami cosa e quando.\n- Domani mattina è piena? Dimmelo stasera dopo le 20.\n- C\'è qualcosa che ti ho chiesto di ricordarmi oggi?',
  };
  const PERM_TOOLS = ['open_app', 'close_app', 'set_volume', 'set_brightness', 'add_event', 'create_file'];
  const PERM_DEF = { add_event: 'ask', create_file: 'ask' };
  let wsFile = 'SOUL.md', perms = {};
  async function wsLoad(f) {
    wsFile = f;
    $('wsfiles').querySelectorAll('.it').forEach((b) => b.classList.toggle('on', b.dataset.f === f));
    $('wsdesc').textContent = t().wsdesc[f]; $('wsstat').textContent = '';
    $('wstext').value = ''; $('wstext').placeholder = t().wsempty;
    try { const r = await fetch('/api/fs/read?path=' + encodeURIComponent(WS_DIR + f), { cache: 'no-store' }); if (r.ok) $('wstext').value = await r.text(); } catch {}
  }
  async function wsWrite(path, text) {
    try { const r = await fetch('/api/fs/write?path=' + encodeURIComponent(path), { method: 'POST', body: text }); return r.ok ? true : (r.status === 401 || r.status === 403 ? 'unpaired' : false); } catch { return false; }
  }
  function paintPerms() {
    $('perms').innerHTML = PERM_TOOLS.map((k) => `<span>${esc(t().permnames[k])}</span><select data-p="${k}">${['allow', 'ask', 'deny'].map((v) => `<option value="${v}"${(perms[k] || PERM_DEF[k] || 'allow') === v ? ' selected' : ''}>${esc(t().permlv[v])}</option>`).join('')}</select>`).join('');
    $('perms').querySelectorAll('select').forEach((sel) => sel.addEventListener('change', async () => {
      perms[sel.dataset.p] = sel.value;
      const r = await wsWrite(WS_DIR + 'permissions.json', JSON.stringify(perms, null, 1));
      $('wsstat').textContent = r === true ? 'permissions.json: ' + t().wssaved : (r === 'unpaired' ? t().pair : t().cantread);
    }));
  }
  async function hbLoad(body) {
    try {
      const r = await fetch('/api/anima/hb', body ? { method: 'POST', body: JSON.stringify(body) } : { cache: 'no-store' });
      if (!r.ok) return;
      const j = await r.json();
      $('hbevery').value = String(j.every);
      $('hbnext').textContent = j.every > 0 ? t().hbnext(j.next) : '';
    } catch {}
  }
  if ($('ws')) {
    $('wsfiles').querySelectorAll('.it').forEach((b) => b.addEventListener('click', () => wsLoad(b.dataset.f)));
    $('wssave').addEventListener('click', async () => {
      const r = await wsWrite(WS_DIR + wsFile, $('wstext').value);
      $('wsstat').textContent = r === true ? wsFile + ': ' + t().wssaved : (r === 'unpaired' ? t().pair : t().cantread);
      if (wsFile === 'HEARTBEAT.md') hbLoad();
    });
    $('wsex').addEventListener('click', () => { if (!$('wstext').value.trim() || confirm(t().wsex + '?')) $('wstext').value = WS_EX[wsFile]; });
    $('hbevery').addEventListener('change', () => hbLoad({ every: +$('hbevery').value }));
    (async () => {
      try { const r = await fetch('/api/fs/read?path=' + encodeURIComponent(WS_DIR + 'permissions.json'), { cache: 'no-store' }); if (r.ok) perms = JSON.parse(await r.text()) || {}; } catch { perms = {}; }
      paintPerms();
    })();
    wsLoad('SOUL.md'); hbLoad();
  }

  // ---- hands-free voice (the device's wake word, /api/anima/wake) ----
  let wakeTimer = 0;
  function paintWake(w) {
    if (!w || !$('wakestat')) return;
    const st = t().wakest[w.state] || w.state;
    $('wakestat').innerHTML = '<b>' + esc(st) + '</b>' + (w.state === 'listening' && w.label ? ' · «' + esc(w.label) + '»' : '') +
      (w.reason ? ' · ' + esc(w.reason) : '') + (w.triggers || w.on ? ' · ' + esc(t().waketrig(w.triggers || 0, w.last)) : '');
    const built = (w.words || []).length > 0 || w.state !== 'unavailable';
    $('wakeon').checked = !!w.on; $('wakeon').disabled = !built && !w.on;
    $('wakewordrow').style.display = (w.words || []).length > 1 ? '' : 'none';
    $('wakeword').innerHTML = (w.words || []).map((x) => `<option value="${esc(x.id)}"${x.id === w.word ? ' selected' : ''}>${esc(x.label)}</option>`).join('');
    $('wakesensrow').style.display = built ? '' : 'none';
    $('wakesens').querySelectorAll('.it').forEach((b) => b.classList.toggle('on', +b.dataset.s === w.sens));
    const route = (w.stt && w.stt.route) || 'none';
    $('wakenote').textContent = !built ? t().wakebuild
      : (w.label ? t().wakehint(w.label) + ' ' : '') + t().wakestt[route] + (route !== 'none' ? (w.stt.where || '') : '');
  }
  async function wakeCall(body) {
    try {
      const r = await fetch('/api/anima/wake?lang=' + lang, body ? { method: 'POST', body: JSON.stringify(body) } : { cache: 'no-store' });
      if (r.ok) paintWake(await r.json());
      else if ($('wakestat')) $('wakestat').textContent = r.status === 404 ? t().wakebuild : 'HTTP ' + r.status;
    } catch {}
  }
  if ($('wakeon')) {
    $('wakeon').addEventListener('change', () => wakeCall({ on: $('wakeon').checked }));
    $('wakeword').addEventListener('change', () => wakeCall({ word: $('wakeword').value }));
    $('wakesens').querySelectorAll('.it').forEach((b) => b.addEventListener('click', () => wakeCall({ sens: +b.dataset.s })));
    // live while the page is visible (the wake service applies a change a moment later)
    const tick = () => { if (!root.isConnected) { clearInterval(wakeTimer); return; } if (!document.hidden) wakeCall(); };
    wakeTimer = setInterval(tick, 3000);
  }
  if (full) { loadNet(); wakeCall(); if ($('gpu')) gpuInit(); }

  return {
    reload, getCfg,
    setLang(l) { lang = l === 'en' ? 'en' : 'it'; /* re-render labels by rebuilding */ mountKeyManager(container, Object.assign({}, opts, { lang })); },
    destroy() { container.innerHTML = ''; },
  };
}
