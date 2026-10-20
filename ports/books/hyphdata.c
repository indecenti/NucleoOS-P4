// hyphdata.c — the hyphenation patterns of the Libri app. Empty until gen_hyph.py writes the
// languages whose pattern licenses allow them in the app (docs/BOOKS_PLAN.md, F2); soft hyphens
// and hard hyphens in the text still break words meanwhile.
#include <stddef.h>

typedef struct { const char *lang; const char *patterns; int left, right; } hyph_lang_t;
const hyph_lang_t bk_hyph[] = {{NULL, NULL, 0, 0}};
