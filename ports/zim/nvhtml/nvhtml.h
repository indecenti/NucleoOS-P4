// nvhtml.h — HTML to plain text for the zim tool and ANIMA (and the base of the reader mode of the
// Biblioteca app). A tolerant tokenizer, no DOM: the article pages of ZIM files (MediaWiki through
// mwoffliner, DevDocs, Stack Exchange, Gutenberg) become headings ("# ", "## "...), paragraphs,
// list items ("- ", "1. ") and table rows (cells joined by " | "). Footnote markers, edit links,
// navigation boxes, hidden elements, scripts and styles are dropped; a formula becomes its TeX.
// The input is untrusted (it comes from a downloaded file): fuzzed in tests/host/fuzz/fuzz_nvhtml.cpp.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int intro;              // only the text before the first section heading (level 2 or deeper)
    const char *section;    // only this section: its heading starts with this text (case-insensitive)
    size_t max;             // at most this many bytes, cut at a paragraph or sentence end + " […]"; 0 = all
    int no_title;           // drop the page title (the h1)
    int back_matter;        // keep Notes, References, External links... (dropped by default)
} nvhtml_opts_t;

// The text of an HTML page: malloc'd and NUL-terminated (free it), or NULL when out of memory.
// opts may be NULL. A section that is not there gives an empty string.
char *nvhtml_text(const char *html, size_t n, const nvhtml_opts_t *opts, size_t *out_len);

// The section headings of a page, one per line ("## History", "### Early years"); malloc'd.
char *nvhtml_sections(const char *html, size_t n, size_t *out_len);

// ISO-8859-1 to UTF-8 (pages of old ZIM files declare it in their MIME type); malloc'd.
char *nvhtml_latin1_to_utf8(const char *s, size_t n, size_t *out_len);

#ifdef __cplusplus
}
#endif
