// Unit tests for nucleo_anima_vault: the public view of teacher.json never carries a key, a page's
// write keeps the stored keys it could not send back (and never moves one to another host), and the
// relay's credential belongs to the request's host only. The keys below are fakes, on purpose not in
// any provider's format.
#include "check.h"
#include "nucleo_anima_vault.h"
#include "cJSON.h"

#include <cstdlib>
#include <cstring>
#include <string>

static const char *kGroq = "TESTKEY-groq-1111222233334444";
static const char *kAnth = "TESTKEY-anth-5555666677778888";
static const char *kVis  = "TESTKEY-vis-999900001111";

static std::string stored_doc() {
    std::string s = "{\"provider\":\"openai\",\"base\":\"https://api.groq.com/openai/v1\",\"model\":\"m\",\"key\":\"";
    s += kGroq;
    s += "\",\"exec\":\"browser\",\"vision_base\":\"https://api.x.ai/v1\",\"vision_key\":\"";
    s += kVis;
    s += "\",\"keys\":{\"openai\":{\"base\":\"https://api.groq.com/openai/v1\",\"model\":\"m\",\"key\":\"";
    s += kGroq;
    s += "\"},\"anthropic\":{\"base\":\"https://api.anthropic.com\",\"model\":\"c\",\"version\":\"2023-06-01\",\"key\":\"";
    s += kAnth;
    s += "\"}}}";
    return s;
}

static std::string take(char *p) {
    std::string s = p ? p : "";
    free(p);
    return s;
}

static bool has(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

static void test_public() {
    const std::string pub = take(nucleo_vault_public(stored_doc().c_str()));
    CHECK(!pub.empty());
    CHECK(!has(pub, kGroq));
    CHECK(!has(pub, kAnth));
    CHECK(!has(pub, kVis));
    CHECK(!has(pub, "TESTKEY-groq-1111"));          // no long prefix either
    CHECK(has(pub, "\"key\":\"\""));
    CHECK(has(pub, "\"key_set\":true"));
    CHECK(has(pub, "\"key_hint\":\"TEST\xE2\x80\xA6" "4444\""));
    CHECK(has(pub, "\"vision_key_set\":true"));
    CHECK(has(pub, "\"exec\":\"browser\""));         // the rest is untouched
    CHECK(has(pub, "\"model\":\"c\""));
    CHECK(nucleo_vault_public("not json") == nullptr);
    // A short key gets no hint characters at all.
    const std::string s = take(nucleo_vault_public("{\"key\":\"abc\"}"));
    CHECK(!has(s, "abc"));
    CHECK(has(s, "\"key_set\":true"));
}

static void test_merge_roundtrip() {
    // A page writes back exactly what it was shown: every stored key survives.
    const std::string st = stored_doc();
    const std::string pub = take(nucleo_vault_public(st.c_str()));
    char err[64] = "";
    const std::string m = take(nucleo_vault_merge(st.c_str(), pub.c_str(), err, sizeof err));
    CHECK(has(m, kGroq));
    CHECK(has(m, kAnth));
    CHECK(has(m, kVis));
    CHECK(!has(m, "key_set"));                         // companions are never stored
    CHECK(!has(m, "key_hint"));
    // An older page that leaves the keys out entirely: same result.
    const std::string bare = "{\"provider\":\"openai\",\"base\":\"https://api.groq.com/openai/v1\",\"model\":\"m2\","
                             "\"vision_base\":\"https://api.x.ai/v1\",\"keys\":{\"openai\":{\"base\":\"https://api.groq.com/openai/v1\",\"model\":\"m2\"},"
                             "\"anthropic\":{\"base\":\"https://api.anthropic.com\",\"model\":\"c\"}}}";
    const std::string m2 = take(nucleo_vault_merge(st.c_str(), bare.c_str(), err, sizeof err));
    CHECK(has(m2, kGroq) && has(m2, kAnth) && has(m2, kVis));
    CHECK(has(m2, "\"m2\""));
}

static void test_merge_new_and_removed() {
    char err[64] = "";
    const std::string st = stored_doc();
    // A key typed by the user replaces the stored one.
    std::string in = "{\"provider\":\"openai\",\"base\":\"https://api.groq.com/openai/v1\",\"key\":\"TESTKEY-new-000011112222\","
                     "\"keys\":{\"openai\":{\"base\":\"https://api.groq.com/openai/v1\",\"key\":\"TESTKEY-new-000011112222\"}}}";
    std::string m = take(nucleo_vault_merge(st.c_str(), in.c_str(), err, sizeof err));
    CHECK(has(m, "TESTKEY-new-000011112222"));
    CHECK(!has(m, kGroq));
    CHECK(!has(m, kAnth));   // the anthropic entry was left out of "keys": it goes, with its key
    // null removes a key; "" or absent keeps it.
    in = "{\"provider\":\"openai\",\"base\":\"https://api.groq.com/openai/v1\",\"key\":null,\"vision_base\":\"https://api.x.ai/v1\",\"vision_key\":\"\","
         "\"keys\":{\"openai\":{\"base\":\"https://api.groq.com/openai/v1\",\"key\":null}}}";
    m = take(nucleo_vault_merge(st.c_str(), in.c_str(), err, sizeof err));
    CHECK(!has(m, kGroq));
    CHECK(has(m, kVis));
    // A provider sent as null goes, entry and key (it was stored as "null" before).
    in = "{\"provider\":\"openai\",\"base\":\"https://api.groq.com/openai/v1\",\"key\":\"\","
         "\"keys\":{\"openai\":{\"base\":\"https://api.groq.com/openai/v1\",\"key\":\"\"},\"anthropic\":null}}";
    m = take(nucleo_vault_merge(st.c_str(), in.c_str(), err, sizeof err));
    CHECK(!has(m, kAnth) && !has(m, "anthropic") && !has(m, "null"));
    CHECK(has(m, kGroq));
    // Not an object: refused with a reason.
    CHECK(nucleo_vault_merge(st.c_str(), "[1,2]", err, sizeof err) == nullptr && err[0]);
    // Nothing stored yet: blanks are dropped, typed keys stored.
    m = take(nucleo_vault_merge(nullptr, "{\"provider\":\"openai\",\"key\":\"\",\"keys\":{}}", err, sizeof err));
    CHECK(!has(m, "\"key\""));
}

static void test_merge_never_moves_a_key() {
    char err[64] = "";
    const std::string st = stored_doc();
    // The active provider switches to anthropic; the page sends no keys. The top-level key must become
    // the anthropic key (from its entry), never the Groq key that was at the top level.
    std::string in = "{\"provider\":\"anthropic\",\"base\":\"https://api.anthropic.com\",\"model\":\"c\",\"key\":\"\","
                     "\"keys\":{\"openai\":{\"base\":\"https://api.groq.com/openai/v1\"},\"anthropic\":{\"base\":\"https://api.anthropic.com\"}}}";
    std::string m = take(nucleo_vault_merge(st.c_str(), in.c_str(), err, sizeof err));
    char n[32], v[96], ver[32];
    CHECK(nucleo_vault_auth_for(m.c_str(), "https://api.anthropic.com/v1/messages", n, sizeof n, v, sizeof v, ver, sizeof ver));
    CHECK(!strcmp(v, kAnth));
    cJSON *root = cJSON_Parse(m.c_str());
    cJSON *top = root ? cJSON_GetObjectItem(root, "key") : nullptr;
    CHECK(cJSON_IsString(top) && !strcmp(top->valuestring, kAnth));
    cJSON_Delete(root);
    // A page points the active entry at another host without retyping the key: the key stays behind.
    in = "{\"provider\":\"openai\",\"base\":\"https://evil.example/v1\",\"key\":\"\","
         "\"keys\":{\"openai\":{\"base\":\"https://evil.example/v1\"}}}";
    m = take(nucleo_vault_merge(st.c_str(), in.c_str(), err, sizeof err));
    CHECK(!has(m, kGroq));
    CHECK(!nucleo_vault_auth_for(m.c_str(), "https://evil.example/v1/chat/completions", n, sizeof n, v, sizeof v, ver, sizeof ver));
    // Same for the vision helper's host.
    in = "{\"provider\":\"openai\",\"base\":\"https://api.groq.com/openai/v1\",\"vision_base\":\"https://evil.example\",\"vision_key\":\"\"}";
    m = take(nucleo_vault_merge(st.c_str(), in.c_str(), err, sizeof err));
    CHECK(!has(m, kVis));
}

static void test_auth_for() {
    const std::string st = stored_doc();
    char n[32], v[96], ver[32];
    CHECK(nucleo_vault_auth_for(st.c_str(), "https://api.groq.com/openai/v1/chat/completions", n, sizeof n, v, sizeof v, ver, sizeof ver));
    CHECK(!strcmp(n, "Authorization"));
    CHECK(!strcmp(v, (std::string("Bearer ") + kGroq).c_str()));
    CHECK(nucleo_vault_auth_for(st.c_str(), "https://API.Anthropic.com:443/v1/messages", n, sizeof n, v, sizeof v, ver, sizeof ver));
    CHECK(!strcmp(n, "x-api-key") && !strcmp(v, kAnth) && !strcmp(ver, "2023-06-01"));
    CHECK(nucleo_vault_auth_for(st.c_str(), "https://api.x.ai/v1/images/generations", n, sizeof n, v, sizeof v, ver, sizeof ver));
    CHECK(!strcmp(v, (std::string("Bearer ") + kVis).c_str()));
    // Look-alike hosts get nothing.
    const char *bad[] = { "https://api.groq.com.evil.example/v1", "https://api.groq.com@evil.example/v1",
                          "http://api.groq.com/openai/v1", "https://api.groq.co/v1", "https://evil.example/?u=https://api.groq.com",
                          "api.groq.com/openai/v1", "", nullptr };
    for (int i = 0; bad[i]; i++) {
        v[0] = 'x';
        CHECK(!nucleo_vault_auth_for(st.c_str(), bad[i], n, sizeof n, v, sizeof v, ver, sizeof ver));
    }
    CHECK(!nucleo_vault_auth_for(nullptr, "https://api.groq.com/", n, sizeof n, v, sizeof v, ver, sizeof ver));
    // A LAN server keeps its port in the match.
    const char *lan = "{\"provider\":\"local\",\"base\":\"http://192.168.1.10:11434/v1\",\"key\":\"TESTKEY-lan-123456789012\"}";
    CHECK(nucleo_vault_auth_for(lan, "http://192.168.1.10:11434/v1/models", n, sizeof n, v, sizeof v, ver, sizeof ver));
    CHECK(!nucleo_vault_auth_for(lan, "http://192.168.1.10:8080/v1/models", n, sizeof n, v, sizeof v, ver, sizeof ver));
    // A key too long for the buffer is not handed out cut.
    char small[16];
    CHECK(!nucleo_vault_auth_for(st.c_str(), "https://api.groq.com/x", n, sizeof n, small, sizeof small, ver, sizeof ver));
}

int main() {
    test_public();
    test_merge_roundtrip();
    test_merge_new_and_removed();
    test_merge_never_moves_a_key();
    test_auth_for();
    return TEST_DONE("vault");
}
