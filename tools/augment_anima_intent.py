"""Grow the intent suggester's training data with a LOCAL LLM on the dev PC's GPU (Ollama).

  ollama pull qwen3.5:9b                                   # once; any multilingual instruct model works
  python tools/augment_anima_intent.py [--model qwen3.5:9b] [--per 30]
      -> tools/anima_intent_aug.txt  (reviewable; read by tools/train_anima_intent.py)

For every canonical request it asks for natural paraphrases (colloquial, indirect, complaints, light typos),
and for HARD negatives: sentences with the same words that are not requests ("il volume di una sfera").
Nothing generated here is ever executed by itself: it only trains the suggester, whose proposals always
wait for a yes. Filters: same direction as the canonical (the trainer's polarity guard), short, deduplicated,
and never a held-out phrase (tools/anima_intent_heldout.txt stays an honest test).
"""
import argparse
import json
import os
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_anima_phrases as gen  # noqa: E402
import train_anima_intent as tr  # noqa: E402

OUT = os.path.join(gen.ROOT, "tools", "anima_intent_aug.txt")
OUT_HARD = os.path.join(gen.ROOT, "tools", "anima_intent_aug_hard.txt")   # --hard

ASK = {
    "it": ("Sei un esperto di linguaggio naturale italiano. Elenca {n} modi DIVERSI e naturali in cui una persona "
           "può chiedere a voce o per iscritto a un assistente di un tablet: «{c}». Varia molto: forme colloquiali, "
           "gentili, brusche, indirette (una lamentela che implica la richiesta), con o senza articoli, qualche lieve "
           "errore di battitura. NON cambiare il significato né la direzione. Massimo 10 parole ciascuna. "
           "Rispondi SOLO con JSON: {{\"frasi\": [\"...\"]}}"),
    "en": ("You are an expert in natural English. List {n} DIFFERENT, natural ways a person could ask a tablet "
           "assistant, by voice or text: \"{c}\". Vary a lot: casual, polite, curt, indirect (a complaint that implies "
           "the request), with light typos. Do NOT change the meaning or the direction. At most 10 words each. "
           "Reply ONLY with JSON: {{\"frasi\": [\"...\"]}}"),
}
NEG = {
    "it": ("Elenca {n} frasi italiane che contengono parole come volume, luce, schermo, musica, ora, casa, foto, rete, "
           "spazio, ma che NON sono richieste di fare qualcosa a un dispositivo: domande di cultura, affermazioni, "
           "ricordi, opinioni (es. «il volume di una sfera», «la luce del sole è calda»). Massimo 10 parole. "
           "Rispondi SOLO con JSON: {{\"frasi\": [\"...\"]}}"),
    "en": ("List {n} English sentences that contain words like volume, light, screen, music, time, home, photo, network, "
           "space, but are NOT requests to operate a device: trivia questions, statements, memories, opinions "
           "(e.g. \"the volume of a sphere\", \"sunlight feels warm\"). At most 10 words each. "
           "Reply ONLY with JSON: {{\"frasi\": [\"...\"]}}"),
}


def ollama(model, prompt, url):
    body = json.dumps({"model": model, "prompt": prompt, "stream": False, "format": "json", "think": False,
                       "options": {"temperature": 0.9, "num_ctx": 4096}}).encode()
    req = urllib.request.Request(url + "/api/generate", data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        text = json.loads(r.read())["response"]
    try:
        v = json.loads(text)
    except ValueError:
        return []
    items = v.get("frasi") if isinstance(v, dict) else v
    return [s.strip() for s in items or [] if isinstance(s, str) and s.strip()]


# --hard: pairs that the suggester confuses, and statements that MENTION a device word without asking.
CONFUSABLE = [
    ("it", "alza il volume", "alza la luminosità"), ("it", "abbassa il volume", "abbassa la luminosità"),
    ("it", "pausa", "chiudi la musica"), ("it", "riprendi la musica", "alza il volume"),
    ("it", "volume al minimo", "chiudi la musica"), ("it", "sei connesso al wifi?", "ciao"),
    ("it", "che ore sono", "che giorno è oggi"), ("it", "torna alla home", "chiudi la musica"),
    ("en", "turn the volume up", "turn the brightness up"), ("en", "turn the volume down", "turn the brightness down"),
    ("en", "pause the song", "stop the music"), ("en", "resume", "turn the volume up"),
    ("en", "are you online", "hello"), ("en", "what time is it", "what day is it today"),
]
PAIR = {
    "it": ("Scrivi {n} frasi naturali con cui una persona chiede a un tablet «{a}» e che NON potrebbero mai voler dire "
           "«{b}»: usa proprio le parole che distinguono le due richieste. Massimo 10 parole. "
           "Rispondi SOLO con JSON: {{\"frasi\": [\"...\"]}}"),
    "en": ("Write {n} natural sentences a person could say to ask a tablet \"{a}\" that could NEVER mean \"{b}\": use the "
           "very words that tell the two apart. At most 10 words. Reply ONLY with JSON: {{\"frasi\": [\"...\"]}}"),
}
HARDNEG = {
    "it": ("Scrivi {n} frasi italiane che NOMINANO volume, luce, luminosità, schermo, musica, canzone, pausa, ora, "
           "casa, rete o wifi ma NON chiedono niente al tablet: racconti al passato («ieri ho abbassato il volume»), "
           "descrizioni, opinioni, domande di cultura, frasi negative («non toccare la musica»). Massimo 10 parole. "
           "Rispondi SOLO con JSON: {{\"frasi\": [\"...\"]}}"),
    "en": ("Write {n} English sentences that MENTION volume, light, brightness, screen, music, song, pause, time, home, "
           "network or wifi but ask the tablet NOTHING: past-tense stories (\"I lowered the volume yesterday\"), "
           "descriptions, opinions, trivia questions, negations (\"don't touch the music\"). At most 10 words. "
           "Reply ONLY with JSON: {{\"frasi\": [\"...\"]}}"),
}
WEAK = ["hello", "are you online", "riprendi la musica", "thank you", "what time is it", "stop the music",
        "set the volume to 0", "what day is it today"]

JUDGE = {
    "it": ("Un assistente di un tablet riceve queste frasi. Per ognuna decidi se chiede ESATTAMENTE questo: «{c}» "
           "(stessa azione, stessa direzione; una lamentela che implica proprio quella richiesta vale). Frasi:\n{items}\n"
           "Rispondi SOLO con JSON: {{\"ok\": [numeri delle frasi giuste]}}"),
    "en": ("A tablet assistant receives these sentences. For each, decide whether it asks for EXACTLY this: \"{c}\" "
           "(same action, same direction; a complaint that implies exactly that request counts). Sentences:\n{items}\n"
           "Reply ONLY with JSON: {{\"ok\": [numbers of the right sentences]}}"),
}
JUDGE_NEG = {
    "it": ("Per ognuna di queste frasi decidi se è una RICHIESTA di fare qualcosa su un tablet (aprire, chiudere, "
           "volume, luce, musica, ora, rete...). Frasi:\n{items}\nRispondi SOLO con JSON: {{\"richieste\": [numeri]}}"),
    "en": ("For each of these sentences decide whether it is a REQUEST to do something on a tablet (open, close, "
           "volume, light, music, time, network...). Sentences:\n{items}\nReply ONLY with JSON: {{\"richieste\": [numbers]}}"),
}


def ollama_json(model, prompt, url, num_gpu=None):
    opts = {"temperature": 0, "num_ctx": 8192}
    if num_gpu is not None:
        opts["num_gpu"] = num_gpu          # layers on the GPU; the rest (MoE experts) stays in system RAM
    body = json.dumps({"model": model, "prompt": prompt, "stream": False, "format": "json", "think": False,
                       "options": opts}).encode()
    req = urllib.request.Request(url + "/api/generate", data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=900) as r:
        try:
            return json.loads(json.loads(r.read())["response"])
        except ValueError:
            return {}


IT_W = {"il", "la", "le", "lo", "di", "del", "della", "che", "per", "con", "non", "un", "una", "alza", "abbassa", "apri",
        "chiudi", "volume", "musica", "luce", "schermo", "sono", "più", "piu", "metti", "fammi", "dimmi", "grazie"}
EN_W = {"the", "a", "an", "of", "to", "is", "are", "please", "turn", "up", "down", "open", "close", "music", "screen",
        "what", "time", "you", "my", "it", "volume", "brightness", "light", "thanks", "make", "stop", "song"}


def lang_ok(phrase, lang):
    """A phrase in the OTHER language than its class is dropped (the generator sometimes switches language)."""
    w = set(tr.c_tokens(phrase))
    it, en = len(w & IT_W - {"volume"}), len(w & EN_W - {"volume"})
    return not (lang == "en" and it > en) and not (lang == "it" and en > it)


def verify(model, url, path=OUT, num_gpu=None):
    """LLM as a judge over tools/anima_intent_aug.txt: a phrasing stays only if the judge agrees it means
    exactly its canonical; a negative stays only if the judge agrees it is NOT a request."""
    head, groups = [], {}
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("#") or "|" not in line:
            head.append(line)
            continue
        lang, rest = line.split(" ", 1)
        canon, phrase = rest.split("|", 1)
        groups.setdefault((lang, canon), []).append(phrase)
    kept, dropped = [], []
    for (lang, canon), phrases in sorted(groups.items()):
        for i in range(0, len(phrases), 20):
            batch = phrases[i:i + 20]
            items = "\n".join("%d. %s" % (j + 1, p) for j, p in enumerate(batch))
            if canon == "-":
                v = ollama_json(model, JUDGE_NEG[lang].format(items=items), url, num_gpu)
                bad = {int(x) for x in (v.get("richieste") or []) if str(x).isdigit()}
                good = [j + 1 for j in range(len(batch)) if j + 1 not in bad]
            else:
                v = ollama_json(model, JUDGE[lang].format(c=canon, items=items), url, num_gpu)
                good = [int(x) for x in (v.get("ok") or []) if str(x).isdigit()]
            for j, p in enumerate(batch):
                ok = j + 1 in good and lang_ok(p, lang)
                (kept if ok else dropped).append("%s %s|%s" % (lang, canon, p))
        print("judged %-34s %d phrases" % (canon if canon != "-" else "- (" + lang + ")", len(phrases)), flush=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for h in head:
            if h.startswith("#"):
                f.write(h + "\n")
        f.write("# Verified by an LLM judge (--verify, %s): %d kept, %d dropped.\n" % (model, len(kept), len(dropped)))
        for line in kept:
            f.write(line + "\n")
    with open(path + ".rejected", "w", encoding="utf-8", newline="\n") as f:
        f.write("# Dropped by the LLM judge (kept for review, not used).\n")
        for line in dropped:
            f.write(line + "\n")
    print("judge: %d kept, %d dropped (see %s.rejected)" % (len(kept), len(dropped), os.path.relpath(path, gen.ROOT)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", default="qwen3.5:9b")
    ap.add_argument("--per", type=int, default=30, help="phrasings asked per canonical")
    ap.add_argument("--url", default="http://127.0.0.1:11434")
    ap.add_argument("--verify", action="store_true", help="only run the LLM judge over the existing file")
    ap.add_argument("--hard", action="store_true", help="confusable pairs + hard negatives -> anima_intent_aug_hard.txt")
    ap.add_argument("--judge-model", help="model for --verify (default --model); a strong MoE judge works well, e.g. "
                                          "qwen3.6:35b-a3b-mtp-q4_K_M with --num-gpu 16 on an 8 GB GPU")
    ap.add_argument("--num-gpu", type=int, help="layers the judge keeps on the GPU (the rest runs from system RAM)")
    ap.add_argument("--out", help="write this generation round to tools/anima_intent_aug_<OUT>.txt instead")
    ap.add_argument("--file", help="with --verify: judge tools/anima_intent_aug_<FILE>.txt")
    a = ap.parse_args()
    global OUT
    if a.out:
        OUT = os.path.join(gen.ROOT, "tools", "anima_intent_aug_%s.txt" % a.out)
    if a.verify:
        path = os.path.join(gen.ROOT, "tools", "anima_intent_aug_%s.txt" % a.file) if a.file else (OUT_HARD if a.hard else OUT)
        verify(a.judge_model or a.model, a.url, path, a.num_gpu)
        return
    _f, table, cases, _s = gen.build()
    canon_lang = {}
    for lang, _p, canon in cases:
        canon_lang[canon] = lang
    held = set()
    for line in open(tr.HELD, encoding="utf-8"):
        if "|" in line and not line.startswith("#"):
            held.add(" ".join(tr.c_tokens(line.rstrip("\n").split("|", 1)[1])))
    seen = set(held) | {" ".join(tr.c_tokens(p)) for _l, p, _c in cases} | {" ".join(tr.c_tokens(c)) for c in canon_lang}
    out, dropped = [], 0

    def keep(lang, canon, s):
        nonlocal dropped
        k = " ".join(tr.c_tokens(s))
        if not k or k in seen or len(k.split()) > 12 or (canon != "-" and not tr.compatible(s, canon)):
            dropped += 1
            return
        seen.add(k)
        out.append("%s %s|%s" % (lang, canon, s))

    for path in tr.aug_files():                          # never repeat what any earlier round already has
        for line in open(path, encoding="utf-8"):
            if "|" in line and not line.startswith("#"):
                seen.add(" ".join(tr.c_tokens(line.rstrip("\n").split("|", 1)[1])))
    if a.hard:
        for line in open(tr.AUG, encoding="utf-8"):         # (kept for clarity: the first round)
            if "|" in line and not line.startswith("#"):
                seen.add(" ".join(tr.c_tokens(line.rstrip("\n").split("|", 1)[1])))
        for lang, x, y2 in CONFUSABLE:
            for c, other in ((x, y2), (y2, x)):
                if c in canon_lang:
                    for s_ in ollama(a.model, PAIR[lang].format(n=15, a=c, b=other), a.url):
                        keep(lang, c, s_)
            print("pair %-26s / %-26s %d so far" % (x, y2, len(out)), flush=True)
        for c in WEAK:
            if c in canon_lang:
                for s_ in ollama(a.model, ASK[canon_lang[c]].format(n=30, c=c), a.url):
                    keep(canon_lang[c], c, s_)
        for lang in ("it", "en"):
            for _ in range(6):
                for s_ in ollama(a.model, HARDNEG[lang].format(n=40), a.url):
                    keep(lang, "-", s_)
        with open(OUT_HARD, "w", encoding="utf-8", newline="\n") as f:
            f.write("# GENERATED by tools/augment_anima_intent.py --hard with %s: confusable pairs, weak classes, hard negatives.\n" % a.model)
            f.write("# <lang> <canonical>|<phrase>. Judge it next: --hard --verify.\n")
            for line in out:
                f.write(line + "\n")
        print("%d kept, %d dropped -> %s" % (len(out), dropped, os.path.relpath(OUT_HARD, gen.ROOT)))
        return
    for i, (canon, lang) in enumerate(sorted(canon_lang.items())):
        got = ollama(a.model, ASK[lang].format(n=a.per, c=canon), a.url)
        for s in got:
            keep(lang, canon, s)
        print("[%2d/%d] %-34s %d asked, %d so far" % (i + 1, len(canon_lang), canon, len(got), len(out)), flush=True)
    for lang in ("it", "en"):
        for _ in range(4):
            for s in ollama(a.model, NEG[lang].format(n=40), a.url):
                keep(lang, "-", s)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# GENERATED by tools/augment_anima_intent.py with %s (local LLM) - review freely, delete bad lines.\n" % a.model)
        f.write("# <lang> <canonical>|<phrase>  (\"-\" = not a request). Training data for the suggester only.\n")
        for line in out:
            f.write(line + "\n")
    print("%d kept, %d dropped (direction, duplicate, held-out, length) -> %s" % (len(out), dropped, os.path.relpath(OUT, gen.ROOT)))


if __name__ == "__main__":
    main()
