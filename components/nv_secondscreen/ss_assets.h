// ss_assets — NucleoCast web page and PC script, kept compressed in flash, inflated once into PSRAM.
// NULL if inflating fails (out of PSRAM). The text is NUL-terminated; *len excludes the NUL.
#pragma once
#include <stddef.h>
#include <stdint.h>

const char *ss_asset_cast_html(size_t *len);
const char *ss_asset_nucleocast_py(size_t *len);
