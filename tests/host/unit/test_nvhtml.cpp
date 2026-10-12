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

static std::string rich(const std::string &html) {
    nvhtml_opts_t o = {};
    o.rich = 1;
    return text(html, &o);
}

#include "nvhtml_rich_check.h"

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

    // plain text never carries a control byte (ANIMA and the Terminal read it)
    CHECK(nvhtml_plain_check(all.data(), all.size()) && nvhtml_plain_check(full.data(), full.size()));

    // rich text for the reader: inline styles and links, closed and reopened at every line break
    CHECK(rich("<p>Il <b>gatto</b> \xC3\xA8 <a href=\"Felis_catus\">un felino</a>.</p>") ==
          "Il \x01gatto\x02 \xC3\xA8 \x05" "Felis_catus\x06un felino\x07.");
    CHECK(rich("<p><b>uno</p><p>due</b></p>") == "\x01uno\x02\n\n\x01" "due\x02");
    CHECK(rich("<a href=\"x\">uno<br>due</a>") == "\x05x\x06uno\x07\n\x05x\x06" "due\x07");
    CHECK(rich("<a href=\"A&amp;B?x=1&#38;y\">t</a>") == "\x05" "A&B?x=1&y\x06t\x07");
    CHECK(rich("<a href=\"\">vuoto</a> <a>senza</a>") == "vuoto senza");   // no href: no link
    CHECK(rich("<a href=\"a\">uno</a> <a href=\"b\">due</a><a href=\"c\">tre</a>") ==   // one link per word
          "\x05" "a\x06uno\x07 \x05" "b\x06" "due\x07\x05" "c\x06tre\x07");
    CHECK(rich("<a href=\"a\">uno <a href=\"b\">due</a></a>") == "\x05" "a\x06uno\x07 \x05" "b\x06" "due\x07");
    CHECK(rich("<p>H<sub>2</sub>O, km<sup>2</sup>, <code>x++</code></p>") ==
          "H\x12" "2\x13O, km\x10" "2\x11, \x0Ex++\x0F");
    CHECK(rich("<p>E: <math alttext=\"{\\displaystyle E=mc^{2}}\"></math>.</p>") == "E: \x0E" "E=mc^{2}\x0F.");
    CHECK(rich("<p><i>uno</i> <em>due</em> tre</p>") == "\x03uno due\x04 tre");   // one run, the space inside
    // headings: plain text and the id at the end, as the target of "#id" links
    CHECK(rich("<h2 id=\"Storia_antica\"><b>Storia</b> <a href=\"x\">antica</a></h2><p>x</p>") ==
          "## Storia antica\x19Storia_antica\n\nx");
    CHECK(rich("<h2><span class=\"mw-headline\" id=\"Note_x\">Note x</span></h2>") == "## Note x\x19Note_x");
    CHECK(rich("<h3 id=\"a\"><span class=\"mw-editsection\">modifica</span></h3><p>b</p>") == "b");   // empty
    CHECK(rich("<h2 id=\"x\">uno<br>due</h2>") == "## uno due\x19x");
    // lists, definitions, preformatted text
    CHECK(rich("<ul><li>uno</li><li>due<ol><li>a</li></ol></li></ul>") == "\x17- uno\n\x17- due\n\x17  1. a");
    CHECK(rich("<dl><dt>Termine</dt><dd>definizione</dd></dl>") == "\x01Termine\x02\n\x16" "definizione");
    CHECK(rich("<pre>  a <b>b</b>\n    c\n</pre><p>d</p>") == "\x1D  a \x01" "b\x02\n\x1D    c\n\nd");
    // tables: one line per row, cells separated, header cells in bold, a <br> in a cell is a space
    CHECK(rich("<table><tr><th>Regno</th><td>Animalia</td></tr><tr><td>Peso</td><td>4<br>kg</td></tr></table>") ==
          "\x1C\x01Regno\x02\x1F" "Animalia\n\x1CPeso\x1F" "4 kg");
    // a table in a cell: its own rows, then the rest of the cell on a row line of its own
    CHECK(rich("<table><tr><td>a<table><tr><td>b</td><td>c</td></tr></table>e</td><td>d</td></tr></table>") ==
          "\x1C" "a\n\x1C" "b\x1F" "c\n\x1C" "e\x1F" "d");
    CHECK(rich("<table><tr><td><table><tr><td><table><tr><td>x</td><td>y</td></tr></table></td></tr></table></td></tr></table>") ==
          "\x1C\n\x1C\n\x1Cx\x1Fy\n\x1C\n\x1C");   // deeper: the same, level by level (empty rows are skipped)
    CHECK(rich("<table><tr><td><ul><li>x</li><li>y</li></ul></td><td><p>z</p></td></tr></table>") ==
          "\x1C\xE2\x80\xA2 x \xE2\x80\xA2 y\x1Fz");
    CHECK(rich("<table><td>a</td><td>b</td></table>") == "\x1C" "a\x1F" "b");   // no <tr>
    // pictures: src, declared size (px or ex), a formula's vertical-align, flags, alt; plain text keeps the TeX
    const std::string math = "<p>Formula <img src=\"./_assets_/x/f.svg\" class=\"mwe-math-fallback-image-inline\" "
                             "style=\"vertical-align: -1.005ex; width:20.66ex; height:2.843ex;\" "
                             "alt=\"{\\displaystyle E=mc^{2}}\"> qui.</p>";
    CHECK(rich(math) == "Formula \x15./_assets_/x/f.svg\x1E" "20.66ex\x1E" "2.843ex\x1E-1.005ex\x1Em\x1E" "E=mc^{2}\x14 qui.");
    CHECK(text(math) == "Formula E=mc^{2} qui.");
    CHECK(rich("<a href=\"Cat\"><img src=\"./a/b.png\" width=\"50\" height=\"50\"></a>") ==
          "\x05" "Cat\x06\x15./a/b.png\x1E" "50px\x1E" "50px\x1E\x1E\x1E\x14\x07");   // a picture as a link
    CHECK(rich("<figure><img src=\"p.webp\" width=\"220\" height=\"165\"><figcaption>Didascalia</figcaption></figure>") ==
          "\x15p.webp\x1E" "220px\x1E" "165px\x1E\x1E" "f\x1E\x14\n\nDidascalia");
    CHECK(rich("<p>a <img src=\"x.gif\" width=\"1\" height=\"1\"> b</p>") == "a b");      // a pixel beacon
    CHECK(rich("<p>a <img src=\"x.gif\"> b<img src=\"y.gif\">c</p>") ==
          "a \x15x.gif\x1E\x1E\x1E\x1E\x1E\x14 b\x15y.gif\x1E\x1E\x1E\x1E\x1E\x14" "c");
    CHECK(rich("<h2 id=\"h\">Titolo <img src=\"i.png\"></h2>") == "## Titolo\x19h");     // not in headings
    CHECK(rich("<p><img alt=\"senza src\"></p>").empty());
    CHECK(rich("<img src=\"a.png\" style=\"width: 50%; height:auto\">") == "\x15" "a.png\x1E\x1E\x1E\x1E\x1E\x14");
    CHECK(text("<p>a <img src=\"x.png\"> b</p>") == "a b");                              // plain text: no pictures

    // the page: the infobox after the lead, back matter dropped by its heading name
    nvhtml_opts_t ro = {};
    ro.rich = 1;
    const std::string rp = text(page, &ro);
    const size_t rlead = rp.find("Il \x01gatto\x02"), rbox = rp.find("\x1C\x01Regno\x02\x1F" "Animalia");
    CHECK(rlead != std::string::npos && rbox != std::string::npos && rlead < rbox && rbox < rp.find("## Storia"));
    CHECK(!has(rp, "Fonte") && !has(rp, "Sito"));
    CHECK(rich("<p>a</p><h2 id=\"Note\">Note</h2><p>fonte</p><h2 id=\"Storia\">Storia</h2><p>b</p>") ==
          "a\n\n## Storia\x19Storia\n\nb");
    for (const std::string &r : {rp, rich(page + "<h2 id=\"z\">Z</h2>")}) {
        const char *why = nvhtml_rich_check(r.data(), r.size());
        CHECK(why == nullptr);
        if (why) fprintf(stderr, "  rich text: %s\n", why);
    }
    ro.section = "storia";   // sections work on rich text too
    const std::string rs = text(page, &ro);
    CHECK(rs.rfind("## Storia", 0) == 0 && has(rs, "Venerato.") && !has(rs, "Biologia"));

    // ISO-8859-1 pages of old files
    size_t ln;
    char *u = nvhtml_latin1_to_utf8("caf\xe9 \xff", 6, &ln);
    CHECK(u && std::string(u, ln) == "caf\xC3\xA9 \xC3\xBF");
    free(u);
    return TEST_DONE("nvhtml");
}
