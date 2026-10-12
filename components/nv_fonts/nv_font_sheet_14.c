/*******************************************************************************
 * Size: 14 px
 * Bpp: 4
 * Opts: --bpp 4 --size 14 --no-compress --no-prefilter --font D:\NucleoV2\managed_components\lvgl__lvgl\scripts\built_in_font\Montserrat-Medium.ttf -r 0x20AC,0x2026,0x2013,0x2014,0x2018-0x201E,0x2212 --font C:\Windows\Fonts\DejaVuSansMono.ttf -r 0x3A3,0x2260,0x2264-0x2265,0x221E --format lvgl --force-fast-kern-format --lv-font-name nv_font_sheet_14 -o nv_font_sheet_14.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef NV_FONT_SHEET_14
#define NV_FONT_SHEET_14 1
#endif

#if NV_FONT_SHEET_14

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+03A3 "Σ" */
    0x2f, 0xff, 0xff, 0xfc, 0xd, 0xa3, 0x33, 0x32,
    0x2, 0xf5, 0x0, 0x0, 0x0, 0x4f, 0x30, 0x0,
    0x0, 0x7, 0xe1, 0x0, 0x0, 0x7, 0xe1, 0x0,
    0x0, 0x4f, 0x30, 0x0, 0x2, 0xf5, 0x0, 0x0,
    0xd, 0xa3, 0x33, 0x32, 0x2f, 0xff, 0xff, 0xfc,

    /* U+2013 "–" */
    0xff, 0xff, 0xff, 0xf0,

    /* U+2014 "—" */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,

    /* U+2018 "‘" */
    0x9, 0x50, 0xe1, 0x3f, 0x34, 0xf6, 0x2, 0x0,

    /* U+2019 "’" */
    0x2e, 0x52, 0xf7, 0xd, 0x21, 0xd0, 0x1, 0x0,

    /* U+201A "‚" */
    0x1, 0x3, 0xf6, 0x1e, 0x60, 0xe1, 0x2c, 0x0,

    /* U+201C "“" */
    0x9, 0x50, 0xe0, 0xe, 0x14, 0xb0, 0x3f, 0x39,
    0xd0, 0x4f, 0x69, 0xf1, 0x2, 0x0, 0x10,

    /* U+201D "”" */
    0x2e, 0x57, 0xe0, 0x2f, 0x77, 0xf1, 0xd, 0x23,
    0xc0, 0x1d, 0x7, 0x70, 0x1, 0x1, 0x0,

    /* U+201E "„" */
    0x1, 0x0, 0x10, 0x3f, 0x69, 0xf1, 0x1e, 0x66,
    0xf0, 0xe, 0x14, 0xb0, 0x2c, 0x7, 0x60,

    /* U+2026 "…" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0x3f, 0x50, 0xe9,
    0xa, 0xd0, 0x2e, 0x40, 0xd8, 0xa, 0xc0,

    /* U+20AC "€" */
    0x0, 0x0, 0x7c, 0xff, 0xc6, 0x0, 0x1, 0xdf,
    0x96, 0x68, 0xe7, 0x0, 0xbd, 0x10, 0x0, 0x0,
    0x7, 0xdf, 0xed, 0xdd, 0xc0, 0x0, 0x2, 0xf4,
    0x0, 0x0, 0x0, 0x7, 0xdf, 0xed, 0xdd, 0xc0,
    0x0, 0x0, 0xcc, 0x0, 0x0, 0x0, 0x0, 0x5,
    0xf7, 0x0, 0x0, 0x42, 0x0, 0x8, 0xfb, 0x66,
    0xaf, 0x70, 0x0, 0x4, 0xbf, 0xfc, 0x50,

    /* U+2212 "−" */
    0x3, 0x33, 0x33, 0x30, 0xe, 0xee, 0xee, 0xe2,

    /* U+221E "∞" */
    0x1a, 0xd5, 0x1b, 0xc3, 0x8, 0x82, 0xdb, 0x72,
    0xd0, 0xa2, 0x7, 0xd0, 0xb, 0x19, 0x50, 0xce,
    0x30, 0xe0, 0x2d, 0xe8, 0x2e, 0xe7, 0x0, 0x1,
    0x0, 0x1, 0x0,

    /* U+2260 "≠" */
    0x0, 0x0, 0x0, 0x10, 0x0, 0x0, 0x0, 0xd6,
    0x3, 0x33, 0x3a, 0xd3, 0x4f, 0xff, 0xff, 0xfe,
    0x0, 0x3, 0xf3, 0x0, 0x15, 0x5d, 0xc5, 0x55,
    0x3d, 0xfe, 0xdd, 0xdb, 0x7, 0xe1, 0x0, 0x0,
    0xa, 0x30, 0x0, 0x0,

    /* U+2264 "≤" */
    0x0, 0x0, 0x0, 0x4, 0x0, 0x1, 0x5a, 0xfd,
    0x16, 0xbf, 0xd8, 0x30, 0x4f, 0xe6, 0x0, 0x0,
    0x3, 0x8d, 0xfb, 0x61, 0x0, 0x0, 0x27, 0xce,
    0x2, 0x22, 0x22, 0x23, 0x4f, 0xff, 0xff, 0xfe,

    /* U+2265 "≥" */
    0x12, 0x0, 0x0, 0x0, 0x4f, 0xd8, 0x30, 0x0,
    0x1, 0x5a, 0xee, 0x94, 0x0, 0x0, 0x19, 0xfe,
    0x3, 0x8c, 0xfb, 0x61, 0x4f, 0xb6, 0x10, 0x0,
    0x13, 0x22, 0x22, 0x22, 0x4f, 0xff, 0xff, 0xfe
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 135, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 40, .adv_w = 112, .box_w = 7, .box_h = 1, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 44, .adv_w = 224, .box_w = 14, .box_h = 1, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 51, .adv_w = 51, .box_w = 3, .box_h = 5, .ofs_x = 0, .ofs_y = 6},
    {.bitmap_index = 59, .adv_w = 51, .box_w = 3, .box_h = 5, .ofs_x = 0, .ofs_y = 6},
    {.bitmap_index = 67, .adv_w = 51, .box_w = 3, .box_h = 5, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 75, .adv_w = 93, .box_w = 6, .box_h = 5, .ofs_x = 0, .ofs_y = 6},
    {.bitmap_index = 90, .adv_w = 93, .box_w = 6, .box_h = 5, .ofs_x = 0, .ofs_y = 6},
    {.bitmap_index = 105, .adv_w = 93, .box_w = 6, .box_h = 5, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 120, .adv_w = 155, .box_w = 10, .box_h = 3, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 135, .adv_w = 181, .box_w = 11, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 190, .adv_w = 130, .box_w = 8, .box_h = 2, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 198, .adv_w = 135, .box_w = 9, .box_h = 6, .ofs_x = 0, .ofs_y = 1},
    {.bitmap_index = 225, .adv_w = 135, .box_w = 8, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 261, .adv_w = 135, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 293, .adv_w = 135, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x1c70, 0x1c71, 0x1c75, 0x1c76, 0x1c77, 0x1c79, 0x1c7a,
    0x1c7b, 0x1c83, 0x1d09, 0x1e6f, 0x1e7b, 0x1ebd, 0x1ec1, 0x1ec2
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 931, .range_length = 7875, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 16, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};

/*-----------------
 *    KERNING
 *----------------*/


/*Map glyph_ids to kern left classes*/
static const uint8_t kern_left_class_mapping[] =
{
    0, 0, 0, 0, 1, 1, 2, 1,
    1, 2, 2, 3, 4, 0, 0, 0,
    0
};

/*Map glyph_ids to kern right classes*/
static const uint8_t kern_right_class_mapping[] =
{
    0, 0, 0, 0, 1, 2, 3, 1,
    2, 3, 3, 4, 5, 0, 0, 0,
    0
};

/*Kern values between classes*/
static const int8_t kern_class_values[] =
{
    6, 6, -4, 0, 0, -4, -4, 0,
    -2, -2, 0, 3, 3, 0, -1, 0,
    0, -2, 2, 1
};


/*Collect the kern class' data in one place*/
static const lv_font_fmt_txt_kern_classes_t kern_classes =
{
    .class_pair_values   = kern_class_values,
    .left_class_mapping  = kern_left_class_mapping,
    .right_class_mapping = kern_right_class_mapping,
    .left_class_cnt      = 4,
    .right_class_cnt     = 5,
};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = &kern_classes,
    .kern_scale = 16,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 1,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t nv_font_sheet_14 = {
#else
lv_font_t nv_font_sheet_14 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 13,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if NV_FONT_SHEET_14*/

