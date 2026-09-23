// Vendored from WASM-4 (https://github.com/aduros/wasm4, runtimes/native/src, commit 9d6c962785cf),
// Copyright (c) Bruno Garcia, ISC license (see LICENSE in this directory).
#pragma once

#include <stdint.h>
#include <stddef.h>

void w4_apuInit ();

void w4_apuTick ();

void w4_apuTone (int frequency, int duration, int volume, int flags);

void w4_apuWriteSamples (int16_t* output, unsigned long frames);
