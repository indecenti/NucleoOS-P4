// ANIMA knowledge tiers on the host: a miniature L1 index (encoder ANE2 + index AKB3) and a miniature
// deductive graph (learned/mind.it.jsonl), written by this test, so the conversational follow-ups that
// depend on them ("dimmi di più", "spiegati meglio", "e Newton?", "e della Spagna?") run in CI instead of
// only on the device. The formats are the ones nucleo_anima_l1.c / nucleo_anima_hdc.c read:
//
//   anima-it-encoder.bin  "ANE2" u32 H, u32 D, u32 ncharn, u32 wordn, u32 charn[ncharn], f32 scale, int8[H*D]
//   anima-it-index.bin    "AKB3" u32 D, u32 K, u32 N, int8 centroids[K*D], dir[K]{u32 start, u32 count},
//                         N x {int8 vec[D], u32 answer offset}, answer records:
//                         u8 action | cstr arg | cstr reply_it | cstr reply_en | cstr detail_it | cstr detail_en
//                         (cstr = u16 length + bytes)
//   learned/mind.it.jsonl {"subject","rel","value"[,"label"]} per line
//
// The encoder table is random but deterministic: a question equal to an indexed key encodes to the same
// vector (cosine 1.0, above the 0.85 gate); an unrelated one does not. That is all the follow-up logic
// needs; the real index's paraphrase quality is measured elsewhere (docs/ANIMA_L0.md).
#include "check.h"
#include "nucleo_anima_kb.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <initializer_list>
#include <algorithm>
extern "C" {
#include "nucleo_anima.h"
#include "anima_fakenet.h"
}
#include "miniz.h"   // tdefl: the pack's raw-deflate blocks (the device inflates them with the ROM tinfl)

namespace {

const uint32_t kH = 4096, kD = 64, kCharN[] = { 3, 4 }, kWordN = 2;

uint32_t fnv1a_tag(uint8_t tag, const char *s, size_t len)
{
    uint32_t h = 0x811c9dc5u;
    h = (h ^ tag) * 0x01000193u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 0x01000193u;
    return h;
}

// l1_words(): lowercase ASCII words, Italian accents folded (the keys below are plain ASCII already).
std::vector<std::string> words(const char *in)
{
    std::vector<std::string> w; std::string cur;
    for (const char *p = in; ; p++) {
        const char c = *p;
        if (c && isalnum((unsigned char)c)) cur += (char)tolower((unsigned char)c);
        else { if (!cur.empty()) { w.push_back(cur); cur.clear(); } if (!c) break; }
    }
    return w;
}

std::vector<int8_t> g_table;      // H x D, deterministic

void make_table()
{
    g_table.resize((size_t)kH * kD);
    uint32_t x = 0x9e3779b9u;
    for (auto &v : g_table) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; v = (int8_t)((int)(x % 127) - 63); }
}

// l1_encode(): sum the rows of the char n-grams of "^w1^w2$" and of the word unigrams + bigrams, L2 -> int8.
std::vector<int8_t> encode(const char *text)
{
    const auto w = words(text);
    std::vector<int32_t> acc(kD, 0);
    auto add = [&](uint32_t id) { id %= kH; for (uint32_t k = 0; k < kD; k++) acc[k] += g_table[(size_t)id * kD + k]; };
    std::string t = "^";
    for (const auto &x : w) { t += x; t += '^'; }
    t.back() = '$';
    for (uint32_t g : kCharN) for (size_t i = 0; i + g <= t.size(); i++) add(fnv1a_tag(1, t.c_str() + i, g));
    for (const auto &x : w) add(fnv1a_tag(2, x.c_str(), x.size()));
    for (size_t i = 0; i + 1 < w.size(); i++) { std::string bg = w[i] + " " + w[i + 1]; add(fnv1a_tag(2, bg.c_str(), bg.size())); }
    double n = 0; for (int32_t a : acc) n += (double)a * a;
    n = std::sqrt(n);
    std::vector<int8_t> v(kD);
    for (uint32_t k = 0; k < kD; k++) { long q = std::lround(acc[k] / n * 127.0); v[k] = (int8_t)(q > 127 ? 127 : q < -127 ? -127 : q); }
    return v;
}

void put_u32(std::string &b, uint32_t v) { for (int i = 0; i < 4; i++) b += (char)((v >> (8 * i)) & 0xFF); }
void put_cstr(std::string &b, const char *s) { const size_t n = strlen(s); b += (char)(n & 0xFF); b += (char)(n >> 8); b.append(s, n); }

struct Card { std::initializer_list<const char *> keys; const char *it, *en, *dit, *den; };

void write_file(const char *path, const std::string &data)
{
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
}

void write_l1(const std::vector<Card> &cards)
{
    make_table();
    std::string enc = "ANE2";
    put_u32(enc, kH); put_u32(enc, kD); put_u32(enc, 2); put_u32(enc, kWordN);
    for (uint32_t g : kCharN) put_u32(enc, g);
    put_u32(enc, 0x3F800000u);                                    // scale 1.0f (unused for ranking)
    enc.append((const char *)g_table.data(), g_table.size());
    write_file("anima_sd/data/anima/anima-it-encoder.bin", enc);

    // answers first, to know their offsets; one cluster (K = 1) holds every vector
    std::vector<std::vector<int8_t>> vecs; std::vector<uint32_t> card_of;
    for (size_t c = 0; c < cards.size(); c++)
        for (const char *k : cards[c].keys) { vecs.push_back(encode(k)); card_of.push_back((uint32_t)c); }
    const uint32_t K = 1, N = (uint32_t)vecs.size();
    const size_t head = 16 + (size_t)K * kD + (size_t)K * 8 + (size_t)N * (kD + 4);
    std::string ans; std::vector<uint32_t> off;
    for (const Card &c : cards) {
        off.push_back((uint32_t)(head + ans.size()));
        ans += (char)0;                                           // action: answer
        put_cstr(ans, ""); put_cstr(ans, c.it); put_cstr(ans, c.en); put_cstr(ans, c.dit); put_cstr(ans, c.den);
    }
    std::string idx = "AKB3";
    put_u32(idx, kD); put_u32(idx, K); put_u32(idx, N);
    std::vector<double> cen(kD, 0);
    for (const auto &v : vecs) for (uint32_t k = 0; k < kD; k++) cen[k] += v[k];
    double n = 0; for (double c : cen) n += c * c;
    n = std::sqrt(n);
    for (uint32_t k = 0; k < kD; k++) idx += (char)(int8_t)std::lround(cen[k] / n * 127.0);
    put_u32(idx, 0); put_u32(idx, N);
    for (size_t i = 0; i < vecs.size(); i++) { idx.append((const char *)vecs[i].data(), kD); put_u32(idx, off[card_of[i]]); }
    idx += ans;
    write_file("anima_sd/data/anima/anima-it-index.bin", idx);
}

// A miniature AKB6 pack (docs/ANIMA_KB.md; the real ones come from tools/kb/akb6.py): one deflate block.
struct KbEnt { const char *title, *qid, *summary, *passages, *facts = ""; };
void write_akb6(const char *path, const char *lang, const std::vector<KbEnt> &ents,
                const std::vector<std::pair<std::string, std::string>> &keys)
{
    std::string blk, ents_sec, qids_sec;
    std::vector<std::pair<std::string, std::string>> qids;
    for (size_t i = 0; i < ents.size(); i++) {
        const KbEnt &e = ents[i];
        std::string rec = std::string(e.title) + "\x1e" + e.qid + "\x1e" + "\x1e" + "\x1e" + e.summary + "\x1e" + e.passages +
                          "\x1e" + e.facts;
        put_u32(ents_sec, 0); put_u32(ents_sec, (uint32_t)blk.size()); put_u32(ents_sec, (uint32_t)rec.size());
        blk += rec;
        if (e.qid[0]) qids.push_back({ e.qid, std::to_string(i) });
    }
    std::sort(qids.begin(), qids.end());
    for (auto &kv : qids) qids_sec += kv.first + "\t" + kv.second + "\n";
    size_t zlen = 0;
    void *z = tdefl_compress_mem_to_heap(blk.data(), blk.size(), &zlen, 128);   // raw deflate, no zlib header
    std::string keys_sec;
    std::vector<std::pair<std::string, std::string>> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    for (auto &kv : sorted) keys_sec += kv.first + "\t" + kv.second + "\n";
    const std::string meta = std::string("{\"attribution\": \"Wikipedia (") + lang + ".wikipedia.org), CC BY-SA 4.0, test\"}";
    const uint64_t head = 64 + 6 * 20, o_meta = head, o_keys = o_meta + meta.size(), o_ents = o_keys + keys_sec.size(),
                   o_qids = o_ents + ents_sec.size(), o_bidx = o_qids + qids_sec.size(), o_blks = o_bidx + 16;
    std::string f = "AKB6";
    f += (char)1; f += (char)0; f += (char)0; f += (char)0; f += std::string(lang, 2); f += std::string(2, '\0');
    put_u32(f, (uint32_t)ents.size()); put_u32(f, (uint32_t)keys.size()); put_u32(f, 1); put_u32(f, 6); put_u32(f, 0);
    f += std::string(32, '\0');
    auto sec = [&](const char *tag, uint64_t off, uint64_t sz) { f.append(tag, 4); put_u32(f, (uint32_t)off); put_u32(f, 0);
                                                                 put_u32(f, (uint32_t)sz); put_u32(f, 0); };
    sec("META", o_meta, meta.size()); sec("KEYS", o_keys, keys_sec.size()); sec("ENTS", o_ents, ents_sec.size());
    sec("QIDS", o_qids, qids_sec.size()); sec("BIDX", o_bidx, 16); sec("BLKS", o_blks, zlen);
    f += meta + keys_sec + ents_sec + qids_sec;
    put_u32(f, (uint32_t)o_blks); put_u32(f, 0); put_u32(f, (uint32_t)zlen); put_u32(f, (uint32_t)blk.size());
    f.append((const char *)z, zlen);
    mz_free(z);
    write_file(path, f);
}

anima_result_t askl(const char *q, const char *lang)
{
    nucleo_anima_try_lock();
    anima_result_t r = nucleo_anima_query(q, lang);
    nucleo_anima_unlock();
    return r;
}

anima_result_t ask(const char *q)
{
    nucleo_anima_try_lock();
    anima_result_t r = nucleo_anima_query(q, "it");
    nucleo_anima_unlock();
    return r;
}

// --probe <dir>: the real packs of <dir> (tools/kb/.cache) under the engine, one "lang|question" per stdin
// line, one session (follow-ups work). A tool for checking a build of the packs, not a test.
int probe(const char *dir)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf anima_sd && mkdir -p anima_sd/data/anima/kb && ln -s %s/*.akb6 anima_sd/data/anima/kb/", dir);
    if (system(cmd) != 0) return 1;
    nucleo_anima_reset_session();
    char line[400];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        if (line[0] == '#') {                                   // "#fr|les miserables": the raw key lookup
            anima_kb_ref_t refs[3]; int n = 0;
            const int kind = nucleo_anima_kb_find(bar + 1, line + 1, refs, 3, &n);
            std::printf("[%s] key '%s' -> kind %d, %d ref(s)\n", line + 1, bar + 1, kind, n);
            for (int i = 0; i < n; i++) {
                char fl[1024] = "";
                nucleo_anima_kb_facts(&refs[i], fl, sizeof fl);
                std::printf("    %d:%u %s | %s\n", refs[i].pack, (unsigned)refs[i].ent, refs[i].title, fl);
            }
            continue;
        }
        const anima_result_t r = askl(bar + 1, line);
        std::printf("[%s] %s\n    -> %s: %s\n       (%s)\n", line, bar + 1, r.intent, r.reply, r.trace);
    }
    system("rm -rf anima_sd");
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--probe")) return probe(argv[2]);
    if (system("rm -rf anima_sd && mkdir -p anima_sd/data/anima/learned anima_sd/data/anima/kb") != 0) return 1;
    write_akb6("anima_sd/data/anima/kb/wikipedia_it_test.akb6", "it", {
        { "Albert Einstein", "Q937", "Albert Einstein (Ulma, 14 marzo 1879 - Princeton, 18 aprile 1955) è stato un fisico tedesco.",
          "È noto per la teoria della relatività.\x1fNel 1921 ricevette il premio Nobel per la fisica." },
        { "Mercurio (astronomia)", "Q308", "Mercurio è il pianeta più interno del sistema solare.", "", "area=74797000" },
        { "Mercurio (divinità)", "Q47423", "Mercurio è una divinità della religione romana.", "" },
        { "Mercurio (elemento chimico)", "Q925", "Il mercurio è l'elemento chimico di numero atomico 80.", "" },
        { "Personaggi di South Park", "", "Questa pagina elenca i personaggi di South Park.", "" },
        { "Napoleone Bonaparte", "Q517", "Napoleone Bonaparte (Ajaccio, 15 agosto 1769 - Longwood, 5 maggio 1821) è stato un politico e militare francese.", "",
          "born=1769-08-15|birthplace=Ajaccio|died=1821-05-05|deathplace=Longwood|gender=m" },
        { "Marie Curie", "Q7186", "Marie Curie è stata una fisica e chimica polacca naturalizzata francese.", "",
          "born=1867-11-07|birthplace=Varsavia|died=1934-07-04|deathplace=Passy|gender=f|occupation=fisica;chimica" },
        { "Francia", "Q142", "La Francia è uno Stato dell'Europa occidentale.", "",
          "capital=Parigi|population=68373433|currency=euro|language=francese|continent=Europa" },
    }, {
        { "albert einstein", "0" }, { "einstein", "0" }, { "mercurio", "~1,~2,~3" },
        { "mercurio astronomia", "1" }, { "mercurio divinita", "2" }, { "mercurio elemento chimico", "3" },
        { "personaggi di south park", "4" }, { "jimbo kern", "^4" },
        { "napoleone bonaparte", "5" }, { "napoleone", "5" }, { "napoleon", "@5" }, { "napoleon bonaparte", "@5" },
        { "marie curie", "6" }, { "curie", "6" }, { "francia", "7" }, { "france", "@7" }, { "frankreich", "@7" },
    });
    // a second, English pack: the same Einstein (Q937) by Wikidata ID, nothing else
    write_akb6("anima_sd/data/anima/kb/wikipedia_en_test.akb6", "en", {
        { "Albert Einstein", "Q937", "Albert Einstein (14 March 1879 - 18 April 1955) was a German-born theoretical physicist.", "" },
    }, { { "albert einstein", "0" } });
    write_l1({
        { { "fisica", "cos e la fisica", "che cos e la fisica", "cosa e la fisica" },
          "La fisica è la scienza che studia la materia, l'energia e le loro interazioni.",
          "Physics is the science of matter, energy and their interactions.",
          "Si divide in meccanica, termodinamica, elettromagnetismo, ottica e fisica moderna.",
          "It covers mechanics, thermodynamics, electromagnetism, optics and modern physics." },
        // a coarse curated card for a question the Wikidata facts answer exactly: the fact must win
        { { "quando e nata marie curie" },
          "Marie Curie è nata/o nel 1867.", "Marie Curie was born in 1867.", "", "" },
        { { "fotosintesi", "cos e la fotosintesi", "che cos e la fotosintesi", "cosa e la fotosintesi" },
          "La fotosintesi è il processo con cui le piante trasformano luce, acqua e anidride carbonica in zuccheri.",
          "Photosynthesis is how plants turn light, water and carbon dioxide into sugars.",
          "Avviene nei cloroplasti grazie alla clorofilla e libera ossigeno come prodotto di scarto.",
          "It happens in chloroplasts thanks to chlorophyll and releases oxygen." },
    });
    write_file("anima_sd/data/anima/learned/mind.it.jsonl",
        "{\"subject\":\"Albert Einstein\",\"rel\":\"born\",\"value\":\"14 marzo 1879\"}\n"
        "{\"subject\":\"Isaac Newton\",\"rel\":\"born\",\"value\":\"4 gennaio 1643\"}\n"
        "{\"subject\":\"Galileo Galilei\",\"rel\":\"born\",\"value\":\"15 febbraio 1564\"}\n"
        "{\"subject\":\"Parigi\",\"rel\":\"capital\",\"value\":\"Francia\"}\n"
        "{\"subject\":\"Madrid\",\"rel\":\"capital\",\"value\":\"Spagna\"}\n"
        "{\"subject\":\"Berlino\",\"rel\":\"capital\",\"value\":\"Germania\"}\n"
        "{\"subject\":\"Lione\",\"rel\":\"located_in\",\"value\":\"Francia\"}\n"
        "{\"subject\":\"Francia\",\"rel\":\"located_in\",\"value\":\"Europa\"}\n");

    CHECK(nucleo_anima_init("it") == ESP_OK);
    fakenet_online(0);
    nucleo_anima_set_net_mode(ANIMA_NET_OFF);

    // Each dialogue starts from a fresh session. want = a fragment of the reply; nullptr intent = must NOT
    // be answered from the knowledge (the old topic or focus is no longer the conversation).
    struct Turn { const char *q; const char *intent; const char *want; };
    struct Dialog { std::initializer_list<Turn> turns; };
    const Dialog dialogs[] = {
        // L1 card, then its detail; a second "dimmi di più" stays on the same card
        { { { "cos'è la fisica", nullptr, "scienza che studia la materia" },
            { "dimmi di più", "l1", "meccanica, termodinamica" },
            { "dimmi di più", "l1", "meccanica, termodinamica" } } },
        { { { "cos'è la fisica", nullptr, "scienza che studia la materia" },
            { "spiegati meglio", "l1", "meccanica" } } },
        { { { "cos'è la fotosintesi", nullptr, "piante trasformano luce" },
            { "dimmi di più", "l1", "cloroplasti" } } },
        // a new request in between: "dimmi di più" asks which topic, never the old one
        { { { "cos'è la fisica", nullptr, "scienza" }, { "apri le note", "open_app", nullptr },
            { "dimmi di più", "more", "Di più su cosa" } } },
        // deductive graph: subject shift chains while the fact question is the last turn
        { { { "quando è nato Einstein", "hdc", "14 marzo 1879" },
            { "e Newton?", "hdc", "4 gennaio 1643" },
            { "e Galileo?", "hdc", "15 febbraio 1564" } } },
        { { { "qual è la capitale della Francia", "hdc", "Parigi" },
            { "e della Spagna?", "hdc", "Madrid" },
            { "e Germania?", "hdc", "Berlino" } } },
        // ...and stops when the conversation moved on
        { { { "quando è nato Einstein", "hdc", "1879" }, { "6x6", "calc", "36" },
            { "e Newton?", "", nullptr } } },
        { { { "qual è la capitale della Francia", "hdc", "Parigi" }, { "che tempo fa a Roma?", "weather", nullptr },
            { "e Londra?", "", nullptr } } },
        // transitive location
        { { { "dove si trova Lione", "pcg", "Europa" } } },
    };
    for (const Dialog &d : dialogs) {
        nucleo_anima_reset_session();
        for (const Turn &t : d.turns) {
            const anima_result_t r = ask(t.q);
            bool ok = (!t.intent || !strcmp(r.intent, t.intent)) && (!t.want || strstr(r.reply, t.want));
            if (t.intent && !t.intent[0]) ok = r.tier == ANIMA_TIER_NONE;      // an honest miss, no old focus
            CHECK(ok);
            if (!ok) std::fprintf(stderr, "  [kb] %s -> intent=%s tier=%d reply=%.160s\n", t.q, r.intent, (int)r.tier, r.reply);
        }
    }
    nucleo_anima_reset_session();

    // OFFLINE ENCYCLOPEDIA (AKB6): answer, passages, "which one?", sections, honesty, precedence.
    {
        const Dialog wiki[] = {
            { { { "chi è Albert Einstein", "wiki", "fisico tedesco" },
                { "dimmi di più", "wiki", "teoria della relatività" },
                { "dimmi di più", "wiki", "premio Nobel" },
                { "dimmi di più", "more", "non dice altro" } } },
            { { { "chi era Einstein?", "wiki", "Ulma" } } },
            { { { "parlami di Albert Einstein", "wiki", "fisico" } } },
            { { { "cos'è il mercurio", "wiki_which", "1) Mercurio (astronomia); 2) Mercurio (divinità); 3) Mercurio (elemento chimico)" },
                { "il terzo", "wiki", "numero atomico 80" } } },
            { { { "cos'è il mercurio", "wiki_which", "Quale?" },
                { "intendevo la divinità", "wiki", "religione romana" } } },
            { { { "cos'è il mercurio", "wiki_which", nullptr },
                { "apri le note", "open_app", nullptr } } },             // a new request drops the question
            { { { "chi è Jimbo Kern", "wiki", "Ne parla la voce «Personaggi di South Park»" } } },
            { { { "chi è Pincopallino Gargamella", "", nullptr } } },     // not in the pack: no answer
            { { { "cos'è la fisica", nullptr, "scienza che studia la materia" } } },   // a curated card comes first
        };
        for (const Dialog &d : wiki) {
            nucleo_anima_reset_session();
            for (const Turn &t : d.turns) {
                const anima_result_t r = ask(t.q);
                bool ok = (!t.intent || !strcmp(r.intent, t.intent)) && (!t.want || strstr(r.reply, t.want));
                if (t.intent && !t.intent[0]) ok = strcmp(r.intent, "wiki") != 0 && strcmp(r.intent, "wiki_which") != 0;
                if (ok && !strcmp(r.intent, "wiki")) ok = strstr(r.trace, "Wikipedia") != nullptr;   // always sourced
                CHECK(ok);
                if (!ok) std::fprintf(stderr, "  [wiki] %s -> intent=%s reply=%.160s trace=%s\n", t.q, r.intent, r.reply, r.trace);
            }
        }
        // languages: a pack in the user's language first; a name in another language (Wikidata) finds the
        // entity; an entity found elsewhere moves to the user's language pack by its Wikidata ID
        struct L { const char *lang, *q, *want, *not_want; };
        const L langs[] = {
            { "es", "¿Quién es Albert Einstein?", "(Wikipedia, en) Albert Einstein", nullptr },   // no es pack: English first
            { "es", "¿Quién fue Napoleón?", "(Wikipedia, it) Napoleone Bonaparte", nullptr },
            { "en", "who is Albert Einstein", "theoretical physicist", "(Wikipedia" },     // its own language: no note
            { "en", "who is Einstein", "theoretical physicist", "(Wikipedia" },           // "einstein" only in it -> Q937 -> en
            { "en", "who was Napoleon", "(Wikipedia, it) Napoleone", nullptr },           // only in the Italian pack
            { "it", "chi è Albert Einstein", "fisico tedesco", "(Wikipedia" },
            { "fr", "qu'est-ce que Napoleone Bonaparte ?", "Napoleone Bonaparte", nullptr },   // "qu'est-ce que" -> "what is"
        };
        for (const L &c : langs) {
            nucleo_anima_reset_session();
            const anima_result_t r = askl(c.q, c.lang);
            const bool ok = !strcmp(r.intent, "wiki") && strstr(r.reply, c.want) && (!c.not_want || !strstr(r.reply, c.not_want));
            CHECK(ok);
            if (!ok) std::fprintf(stderr, "  [wiki/%s] %s -> %s %s\n", c.lang, c.q, r.intent, r.reply);
        }
        nucleo_anima_reset_session();
    }

    // FACTS (Wikidata in the pack records): exact answers, five languages, gender, local dates and numbers
    {
        struct F { const char *lang, *q, *want; };
        const F facts[] = {
            { "it", "quando è nata Marie Curie?",            "Marie Curie è nata il 7 novembre 1867 a Varsavia." },
            { "it", "dove è nata Marie Curie",               "Marie Curie è nata a Varsavia." },
            { "it", "quando è morta Marie Curie",            "Marie Curie è morta il 4 luglio 1934 a Passy." },
            { "it", "che lavoro faceva Marie Curie",         "Professione di Marie Curie: fisica e chimica." },
            { "it", "quanti abitanti ha la Francia?",        "Francia ha 68.373.433 abitanti." },
            { "it", "che moneta si usa in Francia",          "Francia — moneta: euro." },
            { "it", "quanto è grande Mercurio?",             "Mercurio ha una superficie di 74.797.000 km²." },  // only the planet has an area
            { "en", "when was Napoleon born?",               "Napoleone Bonaparte was born on August 15, 1769 in Ajaccio." },
            { "es", "¿Cuántos habitantes tiene Francia?",    "Francia tiene 68.373.433 habitantes." },
            { "fr", "quand est née Marie Curie ?",           "Marie Curie est née le 7 novembre 1867 à Varsavia." },
            { "de", "Wie viele Einwohner hat Frankreich?",   "Francia hat 68.373.433 Einwohner." },
            { "de", "Wann wurde Napoleon geboren?",          "Napoleone Bonaparte wurde am 15. August 1769 in Ajaccio geboren." },
        };
        for (const F &c : facts) {
            nucleo_anima_reset_session();
            const anima_result_t r = askl(c.q, c.lang);
            const bool ok = !strcmp(r.intent, "fact") && strstr(r.reply, c.want) && strstr(r.trace, "Wikidata");
            CHECK(ok);
            if (!ok) std::fprintf(stderr, "  [fact/%s] %s -> %s | %s | %s\n", c.lang, c.q, r.intent, r.reply, r.trace);
        }
        // the same question about another entity: "e Napoleone?"
        nucleo_anima_reset_session();
        askl("quando è nata Marie Curie?", "it");
        anima_result_t r = askl("e Napoleone?", "it");
        CHECK(!strcmp(r.intent, "fact") && strstr(r.reply, "Napoleone Bonaparte è nato il 15 agosto 1769"));
        if (strcmp(r.intent, "fact")) std::fprintf(stderr, "  [fact] e Napoleone? -> %s %s\n", r.intent, r.reply);
        // no such fact: no invented answer (Mercurio has no population; the "which one?" path may ask)
        nucleo_anima_reset_session();
        r = askl("quanti abitanti ha Mercurio", "it");
        CHECK(strcmp(r.intent, "fact") != 0);
        nucleo_anima_reset_session();
    }

    // SUBJECT-LESS FOLLOW-UPS on the entity in play (the device bug: "chi è Federico Faggin" -> right,
    // then "cosa ha fatto?" -> an unrelated L1 card, "Fascism"). The fragment is about the topic, never
    // matched by itself.
    {
        auto said = [](const anima_result_t &r, const char *intent, const char *want) {
            const bool ok = !strcmp(r.intent, intent) && strstr(r.reply, want);
            if (!ok) std::fprintf(stderr, "  [followup] want %s '%s' -> %s: %s\n", intent, want, r.intent, r.reply);
            return ok;
        };
        nucleo_anima_reset_session();
        CHECK(said(askl("chi è Albert Einstein", "it"), "wiki", "fisico tedesco"));
        CHECK(said(askl("cosa ha fatto?", "it"), "wiki", "teoria della relatività"));      // the next passage
        CHECK(said(askl("e poi cosa ha fatto?", "it"), "wiki", "premio Nobel"));          // and the one after
        CHECK(said(askl("quando è morto?", "it"), "wiki", "18 aprile 1955"));            // no fact: the summary has it

        nucleo_anima_reset_session();
        CHECK(said(askl("chi è Marie Curie", "it"), "wiki", "fisica e chimica"));
        CHECK(said(askl("e quando è morta?", "it"), "fact", "Marie Curie è morta il 4 luglio 1934 a Passy."));
        CHECK(said(askl("dove è nata?", "it"), "fact", "Marie Curie è nata a Varsavia."));
        CHECK(said(askl("cosa ha fatto?", "it"), "more", "non dice altro su Marie Curie"));  // honest end

        nucleo_anima_reset_session();                                       // no topic at all: ask, never guess
        CHECK(said(askl("cosa ha fatto?", "it"), "clarify", "Di chi o di cosa parli?"));
        nucleo_anima_reset_session();
    }

    system("rm -rf anima_sd");
    return TEST_DONE("anima_kb");
}
