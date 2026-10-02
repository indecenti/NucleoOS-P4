"""Train ANIMA's offline intent SUGGESTER: when every rule missed, it proposes the closest known request
("Intendi «alza il volume»? (sì/no)"). It never runs anything by itself; a yes does, and is learned.

  python tools/train_anima_intent.py [--negatives-from <NucleoOs checkout>] [--eval-only]

Model: multinomial logistic regression over hashed binary features (words, word pairs, char 3-5-grams),
the fastText-supervised recipe, int8-quantized. Features, hashing (FNV-1a) and tokenization mirror
components/nv_anima/anima_intent.c byte for byte; tests/host/unit/test_anima_nl.cpp checks the C scores
against golden ones written here. Needs numpy + scikit-learn (dev only: the generated C is committed).

Inputs : tools/anima_phrases.txt (+ _ha.txt) expanded by gen_anima_phrases.py = the positives, one class per
         canonical; tools/anima_intent_neg.txt = sentences that are NOT requests (class "-").
Outputs: components/nv_anima/anima_intent_model.c, tests/host/unit/anima_intent_golden.h
Eval   : tools/anima_intent_heldout.txt, never trained on.
"""
import argparse
import json
import os
import sys

import numpy as np
from scipy.sparse import csr_matrix
from sklearn.linear_model import LogisticRegression

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_anima_phrases as gen  # noqa: E402

ROOT = gen.ROOT
NEG = os.path.join(ROOT, "tools", "anima_intent_neg.txt")
HELD = os.path.join(ROOT, "tools", "anima_intent_heldout.txt")
AUG = os.path.join(ROOT, "tools", "anima_intent_aug.txt")
AUG_HARD = os.path.join(ROOT, "tools", "anima_intent_aug_hard.txt")
OUT_C = os.path.join(ROOT, "components", "nv_anima", "anima_intent_model.c")
OUT_G = os.path.join(ROOT, "tests", "host", "unit", "anima_intent_golden.h")
NB = 8192          # hash buckets
MAX_TOK, TOK_LEN = 24, 24
FEAT_TOK = 12      # only the first 12 words make features (IT_FEAT_TOK in anima_intent.c)
P_MIN, MARGIN = 0.45, 0.15   # the C gate (anima_intent.h): below either, no suggestion

FOLD = {0xA0: "a", 0xA1: "a", 0xA2: "a", 0xA8: "e", 0xA9: "e", 0xAA: "e", 0xAC: "i", 0xAD: "i", 0xAE: "i",
        0xB2: "o", 0xB3: "o", 0xB4: "o", 0xB9: "u", 0xBA: "u", 0xBB: "u"}


def c_tokens(s):
    """Exactly a_tokenize() / anima_intent.c: Latin-1 accents folded, ASCII alnum lowercased, rest splits."""
    b = s.encode("utf-8")
    toks, cur, i = [], "", 0
    while True:
        c = b[i] if i < len(b) else 0
        out = None
        if c == 0xC3 and i + 1 < len(b):
            i += 1
            out = FOLD.get(b[i])
        elif 0 < c < 128 and chr(c).isalnum():
            out = chr(c).lower()
        if out:
            if len(cur) < TOK_LEN - 1:
                cur += out
        else:
            if cur and len(toks) < MAX_TOK:
                toks.append(cur)
            cur = ""
            if c == 0:
                break
        i += 1
    return toks


def fnv(s):
    h = 2166136261
    for ch in s.encode("ascii"):
        h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    return h


def features(s):
    t = c_tokens(s)[:FEAT_TOK]
    f = set()
    for w in t:
        f.add(fnv("w:" + w) % NB)
        g = "<" + w + ">"
        for n in (3, 4, 5):
            for i in range(len(g) - n + 1):
                f.add(fnv("c:" + g[i:i + n]) % NB)
    for a, b in zip(t, t[1:]):
        f.add(fnv("b:" + a + "_" + b) % NB)
    return sorted(f)


# ── polarity guard (mirrored in anima_intent.c): a suggestion whose direction contradicts the words said
#    ("più silenzio" vs "alza il volume") is never offered ──
UP = {"alza", "alzare", "alzami", "aumenta", "aumentare", "aumentami", "piu", "su", "forte", "alto", "alta", "chiaro",
      "luminoso", "louder", "up", "raise", "increase", "more", "higher", "brighter", "pump"}
DOWN = {"abbassa", "abbassare", "abbassami", "diminuisci", "riduci", "cala", "giu", "meno", "piano", "basso", "bassa",
        "scuro", "quieter", "down", "lower", "reduce", "decrease", "less", "dim", "dimmer", "darker", "tone", "softer",
        # a LOW state or the silence: "troppo debole" (flipped by TOO) wants it up, "togli / silenzio / minimo" down
        "debole", "deboli", "weak", "minimo", "zero", "muto", "togli", "silenzio", "mute", "silence", "quiet"}
OPEN = {"apri", "aprire", "aprimi", "avvia", "lancia", "open", "launch", "start"}
CLOSE = {"chiudi", "chiudere", "ferma", "spegni", "stoppa", "stop", "close", "basta", "kill", "quit", "exit"}
TOO = {"troppo", "troppa", "too"}
# A STATE ("il volume è basso", "the screen is dark") with no imperative verb and no comparative is a
# complaint: the wish is the opposite of the adjective, like "troppo". "metti più basso" stays a wish.
COPULA = {"e", "is", "sono", "are", "sembra", "seems", "looks"}
VERBS = {"alza", "alzare", "alzami", "aumenta", "abbassa", "abbassare", "abbassami", "diminuisci", "riduci", "cala",
         "metti", "imposta", "fai", "rendi", "turn", "make", "set", "raise", "lower", "increase", "decrease", "put",
         "dim", "brighten", "reduce", "pump", "togli", "spegni"}
COMPAR = {"piu", "meno", "more", "less"}
# Never offer anything for a story or a negation: "ho alzato il volume ieri", "non toccare la musica".
PAST = {"alzato", "abbassato", "aumentato", "diminuito", "aperto", "chiuso", "spento", "acceso", "messo", "ieri",
        "turned", "lowered", "raised", "opened", "closed", "yesterday", "was", "were", "era", "erano", "avevo"}
NEG_W = {"non", "dont", "don", "never", "mai"}


def no_offer(s):
    t = c_tokens(s)
    return bool(set(t) & PAST) or (len(t) > 0 and t[0] in NEG_W)


def polarity(s):
    t = set(c_tokens(s))
    d = (len(t & UP) > 0) - (len(t & DOWN) > 0)
    if t & TOO or (t & COPULA and not t & VERBS and not t & COMPAR):
        d = -d
    o = (len(t & OPEN) > 0) - (len(t & CLOSE) > 0)
    return d, o


def compatible(q, canon):
    (qd, qo), (cd, co) = polarity(q), polarity(canon)
    return not (qd and cd and qd != cd) and not (qo and co and qo != co)


def aug_files():
    """Every generation round: tools/anima_intent_aug.txt and tools/anima_intent_aug_*.txt (never .rejected)."""
    import glob
    return sorted(p for p in glob.glob(os.path.join(ROOT, "tools", "anima_intent_aug*.txt")))


def load_neg():
    return [l.strip() for l in open(NEG, encoding="utf-8") if l.strip() and not l.startswith("#")]


def import_neg(base, table_keys, key):
    """Sentences the NucleoOS evals mark as NOT a device request (nonsense, chit-chat, knowledge)."""
    files = ["eval_halluc_it.jsonl", "eval_halluc_en.jsonl", "eval_halluc2_it.jsonl", "eval_halluc2_en.jsonl",
             "eval_halluc3_it.jsonl", "eval_halluc3_en.jsonl", "eval_bigjunk.jsonl", "eval_traps.jsonl",
             "eval_ood.jsonl", "eval_l1_requests.jsonl", "eval_routing.jsonl", "eval_queries.jsonl"]
    out, seen = [], set()
    for f in files:
        p = os.path.join(base, "tools", "anima", f)
        if not os.path.exists(p):
            continue
        for line in open(p, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            try:
                d = json.loads(line)
            except ValueError:
                continue
            q, exp = d.get("q"), str(d.get("expect", ""))
            if not q or exp.startswith("cmd.") or exp in ("l0", "open_app") or "\n" in q:
                continue
            k = q.strip().lower()
            if k in seen or any(key(l, q) and (l + ":" + key(l, q)) in table_keys for l in ("it", "en")):
                continue
            seen.add(k)
            out.append(q.strip())
    with open(NEG, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# NOT device requests (class \"-\" of tools/train_anima_intent.py): nonsense, chit-chat, knowledge.\n")
        fh.write("# Imported from the NucleoOS (Cardputer) ANIMA eval sets by --negatives-from; add your own below.\n")
        for q in out:
            fh.write(q + "\n")
    print("negatives: %d -> %s" % (len(out), os.path.relpath(NEG, ROOT)))


def dataset():
    fillers, table, cases, _ = gen.build()
    X, y, lang_of = [], [], {}
    for lang, p, canon in cases:
        X.append(p); y.append(canon); lang_of[canon] = lang
    for full, canon in table.items():
        pass
    for canon in sorted(lang_of):
        X.append(canon); y.append(canon)
    for q in load_neg():
        X.append(q); y.append("-")
    for path in aug_files():   # local-LLM phrasings (tools/augment_anima_intent.py); unknown canonicals skipped
        for line in open(path, encoding="utf-8"):
            line = line.rstrip("\n")
            if "|" not in line or line.startswith("#"):
                continue
            canon, phrase = line.split(" ", 1)[1].split("|", 1)
            if canon == "-" or canon in lang_of:
                X.append(phrase); y.append(canon)
    # CONTAMINATION GUARD: a training sentence equal to a held-out one (same tokens) is dropped, so the
    # held-out score always measures phrasings the model never saw.
    held = {" ".join(c_tokens(p)) for _e, _w, p in load_held()}
    keep = [i for i, x in enumerate(X) if " ".join(c_tokens(x)) not in held]
    if len(keep) != len(X):
        print("contamination guard: %d training sentences equal to held-out ones dropped" % (len(X) - len(keep)))
    X = [X[i] for i in keep]; y = [y[i] for i in keep]
    return X, y, lang_of


def matrix(texts):
    rows, cols = [], []
    for i, s in enumerate(texts):
        for f in features(s):
            rows.append(i); cols.append(f)
    return csr_matrix((np.ones(len(rows), dtype=np.float32), (rows, cols)), shape=(len(texts), NB))


class Quant:
    """The int8 model exactly as anima_intent.c scores it."""
    def __init__(self, clf):
        W = clf.coef_.T.astype(np.float64)                     # [NB, NC]
        self.scale = float(np.abs(W).max() / 127.0) or 1.0
        self.w = np.clip(np.round(W / self.scale), -127, 127).astype(np.int8)
        self.b = clf.intercept_.astype(np.float32)
        self.labels = list(clf.classes_)

    def probs(self, s):
        f = features(s)
        acc = self.w[f].astype(np.int32).sum(0) if f else np.zeros(len(self.labels), np.int32)
        z = self.b + np.float32(self.scale) * acc.astype(np.float32)
        z = z - z.max()
        e = np.exp(z)
        return e / e.sum()

    def suggest(self, s, en, lang_of):
        p = self.probs(s)
        order = np.argsort(-p)
        top, second = order[0], order[1]
        lab = self.labels[top]
        if lab == "-" or p[top] < P_MIN or p[top] - p[second] < MARGIN:
            return None, float(p[top])
        if lang_of.get(lab) != ("en" if en else "it") or not compatible(s, lab) or no_offer(s):
            return None, float(p[top])
        return lab, float(p[top])


def table_covered():
    """Held-out phrases the paraphrase table already rewrites: L0 answers them, the suggester never runs."""
    fillers, table, _c, _s = gen.build()
    fset = {l: set(gen.tokens(" ".join(v))) for l, v in fillers.items()}
    key = lambda lang, s: lang + ":" + " ".join(t for t in gen.tokens(s) if t not in fset[lang])
    owned = set(table) | {key(l, c) for l, _p, c in _c}
    return lambda en, phrase: key("en" if en else "it", phrase) in owned


def evaluate(q, lang_of, verbose=True):
    covered = table_covered()
    held = []
    for line in open(HELD, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        lang, rest = line.split(" ", 1)
        want, phrase = rest.split("|", 1)
        held.append((lang == "en", want, phrase))
    right = wrong = none = ood_ok = ood_n = 0
    n_cov = sum(1 for en, want, phrase in held if want != "-" and covered(en, phrase))
    held = [h for h in held if h[1] == "-" or not covered(h[0], h[2])]
    for en, want, phrase in held:
        got, p = q.suggest(phrase, en, lang_of)
        if want == "-":
            ood_n += 1
            ood_ok += got is None
            if got and verbose:
                print("   OOD suggested  %-38s -> %s (%.2f)" % (phrase, got, p))
            continue
        if got == want:
            right += 1
        elif got is None:
            none += 1
        else:
            wrong += 1
            if verbose:
                print("   WRONG          %-38s -> %s (%.2f), want %s" % (phrase, got, p, want))
    n = right + wrong + none
    print("held-out: %d/%d right, %d wrong, %d no suggestion; not-requests left alone %d/%d (+%d already in the table)" %
          (right, n, wrong, none, ood_ok, ood_n, n_cov))
    return right, wrong, ood_n - ood_ok


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def load_held():
    held = []
    for line in open(HELD, encoding="utf-8"):
        line = line.rstrip("\n")
        if line and not line.startswith("#"):
            lang, rest = line.split(" ", 1)
            want, phrase = rest.split("|", 1)
            held.append((lang == "en", want, phrase))
    return held


def write(q, lang_of, golden_texts, right, wrong):
    nc = len(q.labels)
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("// GENERATED by tools/train_anima_intent.py - do not edit. The int8 intent-suggester weights.\n")
        f.write('#include "anima_intent.h"\n\n')
        f.write("const int anima_intent_nb = %d;\nconst int anima_intent_nc = %d;\n" % (NB, nc))
        f.write("const float anima_intent_scale = %.9gf;\n" % q.scale)
        f.write("const char *const anima_intent_label[%d] = {\n" % nc)
        for lab in q.labels:
            f.write("    %s,\n" % cstr(lab))
        f.write("};\nconst unsigned char anima_intent_en[%d] = { %s };\n" %
                (nc, ", ".join("1" if lang_of.get(l) == "en" else "0" for l in q.labels)))
        f.write("const float anima_intent_bias[%d] = { %s };\n" % (nc, ", ".join("%.7gf" % b for b in q.b)))
        f.write("// [bucket][class], row-major\nconst signed char anima_intent_w[%d] = {\n" % (NB * nc))
        flat = q.w.reshape(-1)
        for i in range(0, len(flat), 32):
            f.write("    " + ",".join(str(int(v)) for v in flat[i:i + 32]) + ",\n")
        f.write("};\n")
    with open(OUT_G, "w", encoding="utf-8", newline="\n") as f:
        f.write("// GENERATED by tools/train_anima_intent.py - do not edit. C/Python parity: the suggester's top class\n")
        f.write("// and probability for these sentences, as the trainer computed them.\n#pragma once\n\n")
        f.write("struct IntentGolden { const char *q; const char *top; float p; };\n\nstatic const IntentGolden kIntentGolden[] = {\n")
        for s in golden_texts:
            p = q.probs(s)
            k = int(np.argmax(p))
            f.write("    { %s, %s, %.5ff },\n" % (cstr(s), cstr(q.labels[k]), float(p[k])))
        f.write("};\n\n// tools/anima_intent_heldout.txt (never trained on) and what this model scored on it: the test\n"
                "// fails if a later model gets fewer right or more wrong.\n")
        f.write("struct IntentHeld { bool en; const char *want; const char *q; };\n\nstatic const IntentHeld kIntentHeld[] = {\n")
        covered = table_covered()   # the table answers these on the device: the suggester never sees them
        for en, want, phrase in load_held():
            if want != "-" and covered(en, phrase):
                continue
            f.write("    { %s, %s, %s },\n" % ("true" if en else "false", cstr(want), cstr(phrase)))
        f.write("};\nstatic const int kHeldRightFloor = %d, kHeldWrongCeil = %d;\n" % (right, wrong))
    print("wrote %s (%d classes, %d KB of weights), %s" % (os.path.relpath(OUT_C, ROOT), nc, NB * nc // 1024,
                                                          os.path.relpath(OUT_G, ROOT)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--negatives-from", metavar="NUCLEOOS", help="re-import tools/anima_intent_neg.txt from NucleoOs")
    ap.add_argument("--eval-only", action="store_true", help="train and evaluate, write nothing")
    ap.add_argument("--C", type=float, default=8.0, help="inverse L2 regularization")
    a = ap.parse_args()
    if a.negatives_from:
        fillers, table, _c, _s = gen.build()
        fset = {l: set(gen.tokens(" ".join(v))) for l, v in fillers.items()}
        key = lambda lang, s: " ".join(t for t in gen.tokens(s) if t not in fset[lang])
        import_neg(a.negatives_from, set(table), key)
    X, y, lang_of = dataset()
    clf = LogisticRegression(C=a.C, max_iter=5000).fit(matrix(X), y)
    q = Quant(clf)
    print("trained on %d sentences, %d classes (%d negatives)" % (len(X), len(q.labels), y.count("-")))
    right, wrong, _ood = evaluate(q, lang_of)
    if not a.eval_only:
        held = [p for _e, _w, p in load_held()]
        write(q, lang_of, held[:24] + ["alza il volume", "che ore sono", "turn the volume down", "chi ha dipinto la gioconda"],
              right, wrong + _ood)


if __name__ == "__main__":
    main()
