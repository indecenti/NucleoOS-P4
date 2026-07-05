// nv_usb descriptors — interface/endpoint layout + HID touchscreen report descriptor.
// (Adapted from esp-iot-solution usb_extend_screen, MIT; UAC stripped.)
#pragma once
#include "tusb.h"
#include "sdkconfig.h"

enum {
    REPORT_ID_TOUCH = 1,
    REPORT_ID_MAX_COUNT,
    REPORT_ID_COUNT
};

enum {
    ITF_NUM_VENDOR = 0,
#if CFG_TUD_HID
    ITF_NUM_HID,
#endif
    ITF_NUM_TOTAL,
};

enum {
    EPNUM_DEFAULT = 0,
    EPNUM_VENDOR,
#if CFG_TUD_HID
    EPNUM_HID_DATA,
#endif
    EPNUM_TOTAL
};

#if CFG_TUD_HID
// Windows multi-touch digitizer: 5 fingers, absolute coords in [0..width]x[0..height].
#define TUD_HID_REPORT_DESC_TOUCH_SCREEN(report_id, width, height) \
    HID_USAGE_PAGE   ( HID_USAGE_PAGE_DIGITIZER        ),\
    /* USAGE (Touch Screen) */\
    HID_USAGE        ( 0x04                       ),\
    HID_COLLECTION   ( HID_COLLECTION_APPLICATION ),\
      /* Report ID if any */\
      HID_REPORT_ID ( report_id                 ) \
      /* Input */ \
      /* Finger */ \
      FINGER_USAGE(width, height) \
      FINGER_USAGE(width, height) \
      FINGER_USAGE(width, height) \
      FINGER_USAGE(width, height) \
      FINGER_USAGE(width, height) \
      /* Contact count */\
      HID_USAGE     ( 0x54                                   ),\
      HID_LOGICAL_MAX ( 127                                    ),\
      HID_REPORT_COUNT( 1                                    ),\
      HID_REPORT_SIZE ( 8                                    ),\
      HID_INPUT      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),\
    HID_REPORT_ID ( report_id + 1             ) \
    HID_USAGE (0x55              ),\
    HID_REPORT_COUNT (1               ),\
    HID_LOGICAL_MAX (0x10              ),\
    HID_FEATURE ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),\
    HID_COLLECTION_END \

#define FINGER_USAGE(width, height) \
    HID_USAGE     ( 0x42                                   ),\
    HID_COLLECTION  ( HID_COLLECTION_LOGICAL                 ),\
    HID_USAGE     ( 0x42                                   ),\
    HID_LOGICAL_MIN ( 0x00                                 ),\
    HID_LOGICAL_MAX ( 0x01                                 ),\
    HID_REPORT_SIZE ( 1                                    ),\
    HID_REPORT_COUNT( 1                                    ),\
    HID_INPUT      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),\
    HID_REPORT_COUNT( 7                                    ),\
    HID_INPUT      ( HID_CONSTANT | HID_ARRAY | HID_ABSOLUTE ),\
    HID_REPORT_SIZE ( 8                                    ),\
    HID_USAGE     ( 0x51                                   ),\
    HID_REPORT_COUNT( 1                                    ),\
    HID_INPUT      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),\
    HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP                 ),\
    HID_LOGICAL_MAX_N ( width, 2                           ),\
    HID_REPORT_SIZE ( 16                                    ),\
    HID_UNIT_EXPONENT ( 0x0e                                ),\
    /* Inch,EngLinear */\
    HID_UNIT      ( 0x13                                   ),\
    /* X */\
    HID_USAGE     ( 0x30                                   ),\
    HID_PHYSICAL_MIN ( 0                                   ),\
    HID_PHYSICAL_MAX_N ( width, 2                           ),\
    HID_INPUT      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),\
    /* Y */\
    HID_LOGICAL_MAX_N ( height, 2                           ),\
    HID_PHYSICAL_MAX_N ( height, 2                            ),\
    HID_USAGE     ( 0x31                                   ),\
    HID_INPUT      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),\
    HID_USAGE_PAGE ( HID_USAGE_PAGE_DIGITIZER               ),\
    /* Width */\
    HID_USAGE     ( 0x48                                   ),\
    /* Height */\
    HID_USAGE     ( 0x49                                   ),\
    HID_REPORT_COUNT( 2                                    ),\
    HID_INPUT      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),\
    HID_COLLECTION_END, \

#endif
