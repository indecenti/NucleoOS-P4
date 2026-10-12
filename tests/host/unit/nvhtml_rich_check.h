// The rules of nvhtml's rich text (nvhtml.h), checked line by line by test_nvhtml and fuzz_nvhtml:
// the reader trusts them. Returns NULL when the text follows them, else what is wrong.
#pragma once
#include "nvhtml.h"

#include <cstddef>

static inline const char *nvhtml_rich_check(const char *t, size_t n) {
    static const char kOn[] = {NVHTML_BOLD_ON, NVHTML_ITALIC_ON, NVHTML_CODE_ON, NVHTML_SUP_ON, NVHTML_SUB_ON};
    static const char kOff[] = {NVHTML_BOLD_OFF, NVHTML_ITALIC_OFF, NVHTML_CODE_OFF, NVHTML_SUP_OFF, NVHTML_SUB_OFF};
    size_t ls = 0;   // start of the current line
    while (ls <= n) {
        size_t le = ls;
        while (le < n && t[le] != '\n') le++;
        bool on[5] = {};
        int link = 0;   // 0 none, 1 in the href, 2 in the text
        const bool row = le > ls && t[ls] == NVHTML_ROW;
        const bool heading = le > ls && t[ls] == '#';
        for (size_t i = ls; i < le; i++) {
            const char c = t[i];
            if ((unsigned char)c >= 0x20 || c == '\t') continue;   // a tab in preformatted text
            if (link == 1 && c != NVHTML_LINK_TEXT) return "a marker inside an href";
            bool style = false;
            for (int k = 0; k < 5; k++) {
                if (c == kOn[k]) {
                    if (on[k]) return "a style turned on twice";
                    on[k] = style = true;
                } else if (c == kOff[k]) {
                    if (!on[k]) return "a style turned off while off";
                    on[k] = false;
                    style = true;
                }
            }
            if (style) continue;
            switch (c) {
            case NVHTML_LINK:
                if (link) return "a link inside a link";
                link = 1;
                break;
            case NVHTML_LINK_TEXT:
                if (link != 1) return "link text without a link";
                link = 2;
                break;
            case NVHTML_LINK_END:
                if (link != 2) return "a link end without a link";
                link = 0;
                break;
            case NVHTML_ROW: case NVHTML_ITEM: case NVHTML_DEF: case NVHTML_PRE:
                if (i != ls) return "a line marker inside a line";
                break;
            case NVHTML_CELL:
                if (!row) return "a cell outside a table row";
                break;
            case NVHTML_ANCHOR:
                if (!heading) return "an anchor outside a heading";
                for (size_t j = i + 1; j < le; j++)
                    if ((unsigned char)t[j] < 0x20) return "a marker inside an anchor";
                i = le;
                break;
            default:
                return "a control byte that is no marker";
            }
        }
        for (int k = 0; k < 5; k++)
            if (on[k]) return "a style left on at the end of a line";
        if (link) return "a link left open at the end of a line";
        ls = le + 1;
    }
    return nullptr;
}

// Plain text (opts.rich off) has no control byte but the newline (and tabs in preformatted text).
static inline bool nvhtml_plain_check(const char *t, size_t n) {
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)t[i] < 0x20 && t[i] != '\n' && t[i] != '\t') return false;
    return true;
}
