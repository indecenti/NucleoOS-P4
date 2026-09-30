#pragma once
typedef struct { int down, x, y, back; char shot[48]; } script_frame;
int script_build(int ngames, const char *only);   // -> frames; only = "N" runs game N alone
const script_frame *script_at(int i);             // NULL past the end
