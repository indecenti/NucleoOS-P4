// nvhtml (ports/zim/nvhtml): article pages of ZIM files to plain text for the zim tool and ANIMA.
// Small pages written for each rule: structure, what is dropped, character references, infoboxes
// moved after the lead, sections, the intro, back matter and the length cap.
#include "nvhtml.h"

#include "check.h"

#include <cstdlib>
#include <cstring>
#include <string>

static std::string text(const std::string &html, const nvhtml_opts_t *o = nullptr) {
    size_t n = 0;
    char *t = nvhtml_text(html.data(), html.size(), o, &n);
    CHECK(t != nullptr);
    std::string r = t ? std::string(t, n) : "";
    if (t) CHECK(strlen(t) == n);
    free(t);
    return r;
}

static bool has(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

static std::string full_of(const std::string &page) {
    nvhtml_opts_t o = {};
    o.back_matter = 1;
    return text(page, &o);
}

static const char kPage[] =
    "<!DOCTYPE html><html><head><title>Ignored</title><style>p{color:red}</style>"
    "<script>var x = '<p>no</p>';</script></head><body>"
    "<header><h1>Gatto</h1></header>"
    "<section><table class=\"infobox sinottico\"><tr><th>Regno</th><td>Animalia</td></tr>"
    "<tr><th>Peso</th><td>4<br>kg</td></tr></table>"
    "<p>Il <b>gatto</b> &egrave; un mammifero<sup class=\"mw-ref reference\"><a>[1]</a></sup> "
    "domestico&nbsp;diffuso.</p><p>Formula: <math alttext=\"{\\displaystyle E=mc^{2}}\"><mi>E</mi></math>.</p>"
    "<div class=\"navbox\">Navigazione da togliere</div><span style=\"display: none\">nascosto</span>"
    "</section><section><div class=\"mw-heading mw-heading2\"><h2>Storia<span class=\"mw-editsection\">modifica</span></h2></div>"
    "<p>Addomesticato in antichit&#224;, &#x1F408; compreso.</p>"
    "<ul><li>uno</li><li>due<ol><li>a</li><li>b</li></ol></li></ul>"
    "<h3>Egitto</h3><p>Venerato.</p></section>"
    "<section><h2>Biologia</h2><pre>  codice\n    rientrato</pre><dl><dt>Termine</dt><dd>definizione</dd></dl></section>"
    "<section><h2>Note</h2><ol><li>Fonte</li></ol><ol class=\"references\"><li>Citazione</li></ol></section>"
    "<section><h2>Collegamenti esterni</h2><ul><li>Sito</li></ul></section>"
    "<footer>pie di pagina</footer></body></html>";

int main() {
    const std::string page(kPage);
    const std::string all = text(page);
    // structure
    CHECK(all.rfind("# Gatto\n", 0) == 0);
    CHECK(has(all, "\n## Storia\n") && has(all, "\n### Egitto\n") && has(all, "\n## Biologia\n"));
    CHECK(has(all, "Il gatto è un mammifero domestico diffuso."));   // reference marker dropped, &nbsp;
    CHECK(has(all, "Formula: E=mc^{2}."));                            // TeX of the formula, wrapper removed
    CHECK(has(all, "- uno\n- due\n  1. a\n  2. b"));
    CHECK(has(all, "Addomesticato in antichità, \xF0\x9F\x90\x88 compreso."));
    CHECK(has(all, "  codice\n    rientrato"));                       // <pre> keeps its spaces
    CHECK(has(all, "Termine\n  definizione"));
    // dropped
    for (const char *gone : {"Ignored", "color:red", "var x", "no</p>", "Navigazione", "nascosto", "modifica",
                             "pie di pagina", "[1]", "Fonte", "Citazione", "Sito"}) {
        CHECK(!has(all, gone));
        if (has(all, gone)) fprintf(stderr, "  still there: %s\n", gone);
    }
    CHECK(!has(full_of(page), "Citazione"));   // a references list is dropped even when Notes are kept
    // the infobox comes after the lead's prose, its cells joined, a <br> in a cell is a space
    const size_t lead = all.find("Il gatto"), box = all.find("Regno | Animalia");
    CHECK(lead != std::string::npos && box != std::string::npos && lead < box && box < all.find("## Storia"));
    CHECK(has(all, "Peso | 4 kg"));
    // no blank line at the ends, never two in a row
    CHECK(!all.empty() && all.front() != '\n' && all.back() != '\n' && !has(all, "\n\n\n"));

    // back matter kept on request
    nvhtml_opts_t o = {};
    o.back_matter = 1;
    const std::string full = text(page, &o);
    CHECK(has(full, "## Note") && has(full, "## Collegamenti esterni") && has(full, "- Sito"));

    // intro, sections, title
    o = {};
    o.intro = 1;
    const std::string intro = text(page, &o);
    CHECK(has(intro, "Il gatto") && has(intro, "Regno | Animalia") && !has(intro, "Storia") && !has(intro, "Addomesticato"));
    o = {};
    o.section = "storia";
    const std::string st = text(page, &o);
    CHECK(st.rfind("## Storia", 0) == 0 && has(st, "### Egitto") && has(st, "Venerato.") && !has(st, "Biologia"));
    o.section = "EGI";
    CHECK(text(page, &o) == "### Egitto\n\nVenerato.");
    o.section = "Note";                      // back matter is reachable by name
    CHECK(has(text(page, &o), "1. Fonte"));
    o.section = "Inesistente";
    CHECK(text(page, &o).empty());
    o = {};
    o.no_title = 1;
    CHECK(text(page, &o).rfind("Il gatto", 0) == 0);

    // the length cap: at a paragraph or sentence end, with an ellipsis, on a UTF-8 boundary
    o = {};
    o.max = 120;
    const std::string cut = text(page, &o);
    CHECK(cut.size() <= 120 + 6 && has(cut, " [\xE2\x80\xA6]"));
    for (size_t m = 1; m < 200; m++) {
        o.max = m;
        const std::string c = text("<p>àèìòù àèìòù. Ancora àèìòù è.</p><p>Secondo paragrafo lungo.</p>", &o);
        CHECK(c.size() <= m + 6);
        int cont = 0;   // no torn UTF-8 sequence before the ellipsis
        for (size_t i = 0; i + 6 < c.size() || (i < c.size() && c.size() <= 6); i++) {
            const unsigned char ch = (unsigned char)c[i];
            if (cont) { CHECK((ch & 0xC0) == 0x80); cont--; continue; }
            cont = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
        }
    }

    // sections listing
    size_t n;
    char *secs = nvhtml_sections(page.data(), page.size(), &n);
    CHECK(secs && std::string(secs, n) == "## Storia\n### Egitto\n## Biologia\n## Note\n## Collegamenti esterni\n");
    free(secs);

    // character references and odd markup
    CHECK(text("<p>a &amp; b &lt;c&gt; &quot;d&quot; &#65;&#x42; &bogus; &#xD800; &#0; &amp</p>") ==
          "a & b <c> \"d\" AB &bogus; \xEF\xBF\xBD \xEF\xBF\xBD &amp");
    CHECK(text("<p>shy&shy;phen &#8203;x</p>") == "shyphen x");
    CHECK(text("1 < 2 and 3 > 2") == "1 < 2 and 3 > 2");
    CHECK(text("<!-- <p>commento</p> --><p>vero</p>") == "vero");
    CHECK(text("<P CLASS=x>Maiuscolo</P><BR/>dopo") == "Maiuscolo\n\ndopo");
    CHECK(text("<p>a<p>b<p>c") == "a\n\nb\n\nc");                  // optional end tags
    CHECK(text("<p>aperto <b") == "aperto");                        // a tag cut at the end
    CHECK(text("<div class=\"noprint\"><div>x</div><div>y</div></div>z") == "z");   // nested skip
    CHECK(text("<table><tr><td>a</td><td>b</td></tr><tr><td>c</td><td>d</td></tr></table>") == "a | b\nc | d");
    CHECK(text("") == "" && text("   \n\t ") == "");
    CHECK(text("<script>never closed") == "");
    CHECK(text("<img class=\"mwe-math-fallback-image-inline\" alt=\"{\\displaystyle x^2}\">") == "x^2");

    // ISO-8859-1 pages of old files
    size_t ln;
    char *u = nvhtml_latin1_to_utf8("caf\xe9 \xff", 6, &ln);
    CHECK(u && std::string(u, ln) == "caf\xC3\xA9 \xC3\xBF");
    free(u);
    return TEST_DONE("nvhtml");
}
