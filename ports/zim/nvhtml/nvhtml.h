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
    int rich;               // the reader's text: the same lines with the NVHTML_* markers below
} nvhtml_opts_t;

// Rich text (opts.rich): control bytes, never present in the converted text otherwise (the input's
// own control characters are dropped). Every line is self-contained: styles and links still open at
// a line break are closed before it and opened again on the next line.
//  - inline, toggled on and off: bold, italic, code, superscript, subscript;
//  - a link: NVHTML_LINK, the href as written in the page (entities decoded), NVHTML_LINK_TEXT, the
//    text, NVHTML_LINK_END;
//  - at the start of a line: NVHTML_ROW a table row whose cells are separated by NVHTML_CELL (the
//    header cells in bold); NVHTML_ITEM a list item ("  " per level, then "- " or "N. ");
//    NVHTML_DEF a definition; NVHTML_PRE a preformatted line (not wrapped);
//  - at the end of a heading line: NVHTML_ANCHOR and the heading's id (the target of "#id" links);
//  - an image, in the flow like a word: NVHTML_IMAGE, then five fields separated by NVHTML_FIELD
//    (src as written; width and height as declared, a number and its unit, "px" or "ex", or empty;
//    the vertical-align of a formula in ex, or empty; flags: 'm' a formula, 'd' shown on its own
//    line, 'f' inside a figure), the alt text (a formula's TeX), then NVHTML_IMAGE_END. In plain
//    text a formula is its TeX and other images are left out.
#define NVHTML_BOLD_ON    '\x01'
#define NVHTML_BOLD_OFF   '\x02'
#define NVHTML_ITALIC_ON  '\x03'
#define NVHTML_ITALIC_OFF '\x04'
#define NVHTML_LINK       '\x05'
#define NVHTML_LINK_TEXT  '\x06'
#define NVHTML_LINK_END   '\x07'
#define NVHTML_CODE_ON    '\x0E'
#define NVHTML_CODE_OFF   '\x0F'
#define NVHTML_SUP_ON     '\x10'
#define NVHTML_SUP_OFF    '\x11'
#define NVHTML_SUB_ON     '\x12'
#define NVHTML_SUB_OFF    '\x13'
#define NVHTML_IMAGE_END  '\x14'
#define NVHTML_IMAGE      '\x15'
#define NVHTML_FIELD      '\x1E'
#define NVHTML_DEF        '\x16'
#define NVHTML_ITEM       '\x17'
#define NVHTML_ANCHOR     '\x19'
#define NVHTML_ROW        '\x1C'
#define NVHTML_PRE        '\x1D'
#define NVHTML_CELL       '\x1F'
#define NVHTML_HREF_MAX   512   // longer hrefs are cut (bytes, with the NUL)

// The text of an HTML page: malloc'd and NUL-terminated (free it), or NULL when out of memory.
// opts may be NULL. A section that is not there gives an empty string. With opts.rich the text
// carries the markers above (and opts.max may cut one short: the reader does not use it).
char *nvhtml_text(const char *html, size_t n, const nvhtml_opts_t *opts, size_t *out_len);

// The section headings of a page, one per line ("## History", "### Early years"); malloc'd.
char *nvhtml_sections(const char *html, size_t n, size_t *out_len);

// ISO-8859-1 to UTF-8 (pages of old ZIM files declare it in their MIME type); malloc'd.
char *nvhtml_latin1_to_utf8(const char *s, size_t n, size_t *out_len);

#ifdef __cplusplus
}
#endif
