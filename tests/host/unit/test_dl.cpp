// Unit tests for nv_dl_core: the Metalink 4 parser (what Kiwix's MirrorBrain serves), the job record the
// downloader resumes from, the parts of a big file and the mapping of an app's path to the card.
#include "check.h"
#include "nv_dl_core.h"

#include <cstring>
#include <string>
#include <vector>

// Shaped like a real Kiwix .meta4 (MirrorBrain 2.19), with the mirrors out of order, an http and an
// ftp mirror that must be skipped, an entity in a URL and a comment that looks like a tag.
static const char kMeta4[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<metalink xmlns="urn:ietf:params:xml:ns:metalink">
  <generator>MirrorBrain/2.19.0</generator>
  <origin dynamic="true">https://lb.download.kiwix.org/zim/x/test_it_all_2026-08.zim.meta4</origin>
  <file name="test_it_all_2026-08.zim">
    <size>6915581</size>
    <!-- <url priority="0">https://evil.example/x.zim</url> -->
    <hash type="md5">fcb6d261bda32a52f04c1f5c65dccdd8</hash>
    <hash type="sha-1">02c7354989e0f9b51e849762746a5edcae5fd281</hash>
    <hash type="sha-256">038dd591ae7fb4307209d3a7613b622e2a5b45f9bea46b1ddbe62d0e37788781</hash>
    <pieces length="4194304" type="sha-1">
      <hash>dd74444e0dd89a1913fd82ebb2a1f693bdc3767d</hash>
      <hash>64125a5a26272ea97761a004f899c3363a72dae8</hash>
    </pieces>
    <url location="fr" priority="2">https://mirror.download.kiwix.org/zim/x/test_it_all_2026-08.zim</url>
    <url location="nl" priority="1">https://ftp.nluug.nl/pub/kiwix/zim/x/test_it_all_2026-08.zim</url>
    <url location="de" priority="4">http://ftp.fau.de/kiwix/zim/x/test_it_all_2026-08.zim</url>
    <url location="se" priority="3">https://mirror.accum.se/k/x.zim?a=1&amp;b=2</url>
    <url location="us" priority="5">ftp://ftp.example.org/x.zim</url>
    <metaurl mediatype="torrent" priority="1">https://lb.download.kiwix.org/zim/x/test.zim.torrent</metaurl>
  </file>
  <file name="second.zim"><size>9</size><url priority="1">https://other/second.zim</url></file>
</metalink>
)";

static int parse(const std::string &x, nvdl_meta4_t *m, uint8_t *pieces = nullptr, size_t cap = 0) {
    return nvdl_meta4_parse(x.data(), x.size(), m, pieces, cap);
}

static std::string replaced(std::string s, const std::string &a, const std::string &b) {
    const size_t at = s.find(a);
    if (at != std::string::npos) s.replace(at, a.size(), b);
    return s;
}

static nvdl_job_t sample_job() {
    nvdl_job_t j;
    memset(&j, 0, sizeof j);
    j.id = 7;
    snprintf(j.owner, sizeof j.owner, "biblioteca");
    snprintf(j.title, sizeof j.title, "Wikipedia (italiano)\nTop");
    snprintf(j.dest, sizeof j.dest, "/sdcard/home/zim/wikipedia_it_top_nopic_2026-07.zim");
    snprintf(j.src, sizeof j.src, "https://lb.download.kiwix.org/zim/wikipedia/wikipedia_it_top_nopic_2026-07.zim.meta4");
    j.src_meta4 = true;
    j.prepared = true;
    j.size = 943718400ull + 12345;
    j.has_sha256 = true;
    for (int i = 0; i < 32; i++) j.sha256[i] = (uint8_t)(i * 7);
    j.piece_len = 4194304;
    j.n_pieces = (uint32_t)((j.size + j.piece_len - 1) / j.piece_len);
    j.verified = 5ull * 4194304;
    j.state = NVDL_QUEUED;
    j.created = 1791800000u;
    snprintf(j.error, sizeof j.error, "mirror 2: HTTP 503");
    j.n_urls = 2;
    snprintf(j.urls[0], NVDL_URL_MAX, "https://ftp.nluug.nl/pub/kiwix/zim/wikipedia/x.zim");
    snprintf(j.urls[1], NVDL_URL_MAX, "https://mirror.download.kiwix.org/zim/wikipedia/x.zim");
    return j;
}

static bool roundtrip(const nvdl_job_t &j, nvdl_job_t *back) {
    char buf[4096];
    const int n = nvdl_job_format(&j, buf, sizeof buf);
    return n > 0 && nvdl_job_parse(buf, (size_t)n, back);
}

static bool guest(const char *g, std::string *out = nullptr) {
    char p[NVDL_PATH_MAX];
    const bool ok = nvdl_guest_path("/sdcard/home", g, p, sizeof p);
    if (ok && out) *out = p;
    return ok;
}

int main() {
    // ---- Metalink 4
    nvdl_meta4_t m;
    CHECK(parse(kMeta4, &m) == NVDL_OK);
    CHECK(!strcmp(m.name, "test_it_all_2026-08.zim"));
    CHECK(m.size == 6915581);
    CHECK(m.has_sha256 && m.sha256[0] == 0x03 && m.sha256[31] == 0x81);
    CHECK(m.piece_len == 4194304 && m.n_pieces == 2);              // counted without a buffer
    CHECK(m.n_urls == 3);                                          // http and ftp skipped, the comment too
    CHECK(!strcmp(m.urls[0], "https://ftp.nluug.nl/pub/kiwix/zim/x/test_it_all_2026-08.zim"));
    CHECK(!strcmp(m.urls[1], "https://mirror.download.kiwix.org/zim/x/test_it_all_2026-08.zim"));
    CHECK(!strcmp(m.urls[2], "https://mirror.accum.se/k/x.zim?a=1&b=2"));
    uint8_t pc[40];
    CHECK(parse(kMeta4, &m, pc, sizeof pc) == NVDL_OK && pc[0] == 0xdd && pc[19] == 0x7d && pc[20] == 0x64 && pc[39] == 0xe8);
    CHECK(parse(kMeta4, &m, pc, 39) == NVDL_ETOOBIG);
    CHECK(parse(kMeta4, &m, pc, 20) == NVDL_ETOOBIG);

    // the piece count must cover the size exactly
    CHECK(parse(replaced(kMeta4, "<size>6915581</size>", "<size>9000000</size>"), &m) == NVDL_EPIECES);
    CHECK(parse(replaced(kMeta4, "<size>6915581</size>", "<size>4194304</size>"), &m) == NVDL_EPIECES);
    CHECK(parse(replaced(kMeta4, "<hash>64125a5a26272ea97761a004f899c3363a72dae8</hash>", "<hash>xyz</hash>"), &m) == NVDL_EPIECES);
    CHECK(parse(replaced(kMeta4, "length=\"4194304\"", "length=\"100\""), &m) == NVDL_OK && m.piece_len == 0);   // too small: ignored
    // pieces of another type are ignored: the whole-file SHA-256 remains
    CHECK(parse(replaced(kMeta4, "length=\"4194304\" type=\"sha-1\"", "length=\"4194304\" type=\"sha-256\""), &m) == NVDL_OK && m.piece_len == 0 && m.has_sha256);
    CHECK(parse(replaced(kMeta4, "038dd591ae7fb4307209d3a7613b622e2a5b45f9bea46b1ddbe62d0e37788781", "038d"), &m) == NVDL_EPARSE);

    // no usable mirror / not a metalink / no size / truncated
    std::string only_http = kMeta4;
    for (size_t at; (at = only_http.find(">https://")) != std::string::npos;) only_http.replace(at, 9, ">http://");
    CHECK(parse(only_http, &m) == NVDL_ENOURL);
    CHECK(parse("<html><body>not found</body></html>", &m) == NVDL_EPARSE);
    CHECK(parse("", &m) == NVDL_EPARSE);
    CHECK(nvdl_meta4_parse(nullptr, 0, &m, nullptr, 0) == NVDL_EPARSE);
    CHECK(parse(replaced(kMeta4, "<size>6915581</size>", ""), &m) == NVDL_EPARSE);
    CHECK(parse(replaced(kMeta4, "<size>6915581</size>", "<size>0</size>"), &m) == NVDL_EPARSE);
    CHECK(parse(replaced(kMeta4, "<size>6915581</size>", "<size>99999999999999999999999</size>"), &m) == NVDL_EPARSE);
    CHECK(parse(replaced(kMeta4, "<size>6915581</size>", "<size>-3</size>"), &m) == NVDL_EPARSE);
    CHECK(parse(std::string(kMeta4, strstr(kMeta4, "</file>") - kMeta4), &m) == NVDL_EPARSE);
    // URLs: bad entities, spaces, priorities missing or bogus (last), more than NVDL_URLS_MAX
    CHECK(parse(replaced(kMeta4, "a=1&amp;b=2", "a=1&bogus;b=2"), &m) == NVDL_OK && m.n_urls == 2);
    CHECK(parse(replaced(kMeta4, "a=1&amp;b=2", "a=1 b=2"), &m) == NVDL_OK && m.n_urls == 2);
    CHECK(parse(replaced(kMeta4, "priority=\"1\">https://ftp.nluug", ">https://ftp.nluug"), &m) == NVDL_OK &&
          !strcmp(m.urls[2], "https://ftp.nluug.nl/pub/kiwix/zim/x/test_it_all_2026-08.zim"));
    std::string many = "<metalink><file name=\"m\"><size>5</size>";
    for (int i = 20; i > 0; i--) many += "<url priority=\"" + std::to_string(i) + "\">https://m" + std::to_string(i) + "/f</url>";
    many += "</file></metalink>";
    CHECK(parse(many, &m) == NVDL_OK && m.n_urls == NVDL_URLS_MAX && !strcmp(m.urls[0], "https://m1/f") &&
          !strcmp(m.urls[7], "https://m8/f") && m.piece_len == 0 && !m.has_sha256);
    CHECK(parse("<metalink><file name=\"a/../b\\c\"><size>5</size><url>https://x/f</url></file></metalink>", &m) == NVDL_OK &&
          !strcmp(m.name, "a_.._b_c"));
    CHECK(parse("<m:metalink xmlns:m=\"urn:ietf:params:xml:ns:metalink\"><m:file name='q'><m:size>5</m:size>"
                "<m:url priority='1'>https://x/q</m:url></m:file></m:metalink>", &m) == NVDL_OK && m.size == 5);
    CHECK(parse("<metalink><file name=\"x\"><size>5</size><url>https://</url><url>https:///p</url></file></metalink>", &m) == NVDL_ENOURL);

    // ---- job record
    nvdl_job_t j = sample_job(), b;
    CHECK(roundtrip(j, &b));
    CHECK(b.id == 7 && !strcmp(b.owner, "biblioteca") && !strcmp(b.title, "Wikipedia (italiano) Top"));
    CHECK(!strcmp(b.dest, j.dest) && !strcmp(b.src, j.src) && b.src_meta4 && b.prepared);
    CHECK(b.size == j.size && b.has_sha256 && !memcmp(b.sha256, j.sha256, 32));
    CHECK(b.piece_len == j.piece_len && b.n_pieces == j.n_pieces && b.verified == j.verified);
    CHECK(b.state == NVDL_QUEUED && b.created == j.created && !strcmp(b.error, "mirror 2: HTTP 503"));
    CHECK(b.n_urls == 2 && !strcmp(b.urls[1], j.urls[1]));
    char small[64];
    CHECK(nvdl_job_format(&j, small, sizeof small) == -1);
    nvdl_job_t bad = j;
    snprintf(bad.dest, sizeof bad.dest, "/sdcard/x\ny");
    char buf[4096];
    CHECK(nvdl_job_format(&bad, buf, sizeof buf) == -1);
    bad = j; bad.verified = j.size + 1;                 CHECK(!roundtrip(bad, &b));
    bad = j; bad.n_pieces = j.n_pieces + 1;             CHECK(!roundtrip(bad, &b));
    bad = j; bad.state = NVDL_DONE;                     CHECK(!roundtrip(bad, &b));   // done but not all there
    bad.verified = bad.size;                            CHECK(roundtrip(bad, &b) && b.state == NVDL_DONE);
    bad = j; bad.prepared = false;                      CHECK(!roundtrip(bad, &b));   // progress before the meta4
    bad.verified = 0; bad.size = 0; bad.piece_len = 0; bad.n_pieces = 0; bad.n_urls = 0;
    CHECK(roundtrip(bad, &b) && !b.prepared);
    bad = j; bad.piece_len = 0; bad.n_pieces = 0;       CHECK(roundtrip(bad, &b));    // a plain URL
    bad = j; bad.piece_len = 0;                         CHECK(!roundtrip(bad, &b));
    bad = j; bad.owner[0] = 0;                          CHECK(!roundtrip(bad, &b));
    bad = j; snprintf(bad.urls[0], NVDL_URL_MAX, "http://insecure/x"); CHECK(!roundtrip(bad, &b));
    const int n = nvdl_job_format(&j, buf, sizeof buf);
    CHECK(n > 0);
    std::string rec(buf, (size_t)n);
    CHECK(!nvdl_job_parse(("nvdl 2" + rec.substr(6)).c_str(), rec.size(), &b));
    CHECK(!nvdl_job_parse(replaced(rec, "state queued", "state running").c_str(), rec.size(), &b));
    CHECK(!nvdl_job_parse(replaced(rec, "size ", "size x").c_str(), rec.size() + 1, &b));
    CHECK(!nvdl_job_parse(replaced(rec, "title ", "title \x01").c_str(), rec.size() + 1, &b));
    CHECK(!nvdl_job_parse(replaced(rec, "owner bib", "owner b\tib").c_str(), rec.size() + 1, &b));   // never written: refused
    CHECK(!nvdl_job_parse(replaced(rec, "owner bib", "owner b\x7f" "ib").c_str(), rec.size() + 1, &b));
    CHECK(nvdl_job_parse(replaced(rec, "\n", "\r\n").c_str(), rec.size() + 1, &b));          // CRLF tolerated
    CHECK(nvdl_job_parse((rec + "future_key 42\n").c_str(), rec.size() + 14, &b));        // unknown keys ignored
    std::string urls9 = rec;
    for (int i = 0; i < 7; i++) urls9 += "url https://more/" + std::to_string(i) + "\n";
    CHECK(!nvdl_job_parse(urls9.c_str(), urls9.size(), &b));
    CHECK(!nvdl_job_parse((rec + "title " + std::string(500, 'x') + "\n").c_str(), rec.size() + 507, &b));
    CHECK(!nvdl_job_parse(buf, 0, &b) && !nvdl_job_parse(nullptr, 5, &b));

    // ---- resume points
    j = sample_job();
    CHECK(nvdl_resume_offset(&j) == 5ull * 4194304);
    j.verified += 1000;                                  CHECK(nvdl_resume_offset(&j) == 5ull * 4194304);
    j.verified = j.size;                                 CHECK(nvdl_resume_offset(&j) == j.size);
    j.piece_len = 0; j.verified = NVDL_CHECKPOINT * 3ull + 5; CHECK(nvdl_resume_offset(&j) == NVDL_CHECKPOINT * 3ull);
    j.prepared = false;                                  CHECK(nvdl_resume_offset(&j) == 0);

    // ---- parts
    CHECK(nvdl_parts(1) == 1 && nvdl_parts(NVDL_PART_SIZE) == 1 && nvdl_parts(NVDL_PART_SIZE + 1) == 2);
    CHECK(nvdl_parts(NVDL_PART_SIZE * 3) == 3 && nvdl_parts(NVDL_PART_SIZE * (uint64_t)NVDL_PARTS_MAX) == NVDL_PARTS_MAX);
    CHECK(nvdl_parts(NVDL_PART_SIZE * (uint64_t)NVDL_PARTS_MAX + 1) == 0);
    CHECK(NVDL_PART_SIZE % 4194304 == 0 && NVDL_PART_SIZE < 2147483648ull);
    char pn[64];
    CHECK(nvdl_part_name("/s/w.zim", 0, 1, pn, sizeof pn) && !strcmp(pn, "/s/w.zim"));
    CHECK(nvdl_part_name("/s/w.zim", 0, 3, pn, sizeof pn) && !strcmp(pn, "/s/w.zimaa"));
    CHECK(nvdl_part_name("/s/w.zim", 2, 3, pn, sizeof pn) && !strcmp(pn, "/s/w.zimac"));
    CHECK(nvdl_part_name("/s/w.zim", 27, 40, pn, sizeof pn) && !strcmp(pn, "/s/w.zimbb"));
    CHECK(!nvdl_part_name("/s/w.zim", 3, 3, pn, sizeof pn) && !nvdl_part_name("/s/w.zim", 0, 0, pn, sizeof pn));
    CHECK(!nvdl_part_name("/s/w.zim", 0, 2, pn, 10) && nvdl_part_name("/s/w.zim", 0, 2, pn, 11));
    CHECK(!nvdl_part_name("/s/w.zim", 0, 1, pn, 8) && nvdl_part_name("/s/w.zim", 0, 1, pn, 9));

    // ---- an app's path on the card
    std::string out;
    CHECK(guest("/zim/wikipedia_it_top.zim", &out) && out == "/sdcard/home/zim/wikipedia_it_top.zim");
    CHECK(guest("/a b/c-d_e.f") && guest("/x"));
    CHECK(!guest("zim/x.zim") && !guest("/") && !guest("") && !guest("/zim/") && !guest("//x"));
    CHECK(!guest("/zim//x") && !guest("/../x") && !guest("/zim/../../x") && !guest("/./x") && !guest("/zim/.."));
    CHECK(!guest("/zim\\x") && !guest("/c:x") && !guest("/a*b") && !guest("/a?b") && !guest("/a|b"));
    CHECK(!guest("/a\"b") && !guest("/a<b") && !guest("/a>b") && !guest("/a\tb") && !guest("/a\x7f"));
    CHECK(!guest("/x.") && !guest("/x ") && !guest("/dir./x") && guest("/.hidden"));
    CHECK(!guest(("/" + std::string(121, 'a')).c_str()) && guest(("/" + std::string(120, 'a')).c_str()));
    char tiny[20];
    CHECK(!nvdl_guest_path("/sdcard/home", "/zim/a.zim", tiny, 22 - 4));
    CHECK(nvdl_guest_path("/sdcard/home", "/zim/a.zim", tiny, sizeof tiny) == false);
    char exact[24];
    CHECK(nvdl_guest_path("/sdcard/home", "/zim/a.zim", exact, 23) && !strcmp(exact, "/sdcard/home/zim/a.zim"));
    CHECK(!nvdl_guest_path("/sdcard/home/", "/x", exact, sizeof exact) && !nvdl_guest_path("", "/x", exact, sizeof exact));

    // ---- hex
    uint8_t h[2];
    CHECK(nvdl_hex_decode("0aFf", h, 2) && h[0] == 0x0a && h[1] == 0xff);
    CHECK(!nvdl_hex_decode("0aF", h, 2) && !nvdl_hex_decode("0aFf0", h, 2) && !nvdl_hex_decode("0g00", h, 2));

    return TEST_DONE("dl");
}
