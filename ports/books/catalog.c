// catalog.c — the catalog of free books of the NucleoOS store (docs/BOOKS_PLAN.md §2): F3.
// Until then the button opens nothing but a note.
#include "app.h"

void cat_open(void) { toast(tr(S_CAT_LOADING), NULL); }
void cat_draw(void) {}
int cat_action(int act, int arg) { (void)act; (void)arg; return 0; }
void cat_frame(void) {}
void cat_key(const kev_t *k) { (void)k; }
