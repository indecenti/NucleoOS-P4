// nv_ui_scale — the shared spatial scale for NucleoOS SystemUI + apps.
// One source of truth for radii, spacing, and the touch-target minimum so apps stop
// reinventing magic numbers (the root cause of visual drift across pages). Pure constants,
// safe to include anywhere.
#pragma once

// Corner radii.
#define NV_RAD_SM     12   // rows, chips, small controls
#define NV_RAD_MD     16   // cards, sheets, quick-setting tiles
#define NV_RAD_LG     20   // large surfaces / hero cards

// Spacing ladder (px). Use for padding + gaps; keeps rhythm consistent.
#define NV_SP_1        4
#define NV_SP_2        8
#define NV_SP_3       12
#define NV_SP_4       16
#define NV_SP_5       24

// Minimum comfortable touch target (px). Buttons/rows must be at least this tall/wide.
#define NV_TOUCH_MIN  44
