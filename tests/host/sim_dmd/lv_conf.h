// LVGL for the DMD menu simulator: as on the board (16-bit colour, software renderer, NucleoOS fonts).
#pragma once
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 10
#define LV_DRAW_SW_COMPLEX 1
#define LV_USE_DRAW_SW 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 1
#define LV_USE_LOG 0
#define LV_USE_THEME_DEFAULT 1
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(nv_font_14) LV_FONT_DECLARE(nv_font_20) LV_FONT_DECLARE(nv_font_28) LV_FONT_DECLARE(nv_font_mono_17)
#define LV_FONT_DEFAULT &nv_font_20
#define LV_CACHE_DEF_SIZE 0
