// nv_usb descriptors — device/config/string descriptors for the extend-screen composite.
// The vendor interface string encodes geometry+limits for the Windows IDD driver:
// "<target>udisp0_R{W}x{H}_Ejpg{Q}_Fps{F}_Bl{limit}".
// (Adapted from esp-iot-solution usb_extend_screen, MIT; UAC stripped.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tusb.h"
#include "tusb_config.h"
#include "usb_descriptors.h"
#include "nv_usb_screen.h"
#include "sdkconfig.h"

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    .bDeviceClass       = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass    = TUSB_CLASS_UNSPECIFIED,
    .bDeviceProtocol    = TUSB_CLASS_UNSPECIFIED,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0101,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *) &desc_device;
}

#if CFG_TUD_HID
//--------------------------------------------------------------------+
// HID Report Descriptor
//--------------------------------------------------------------------+
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_TOUCH_SCREEN(REPORT_ID_TOUCH, NV_USB_SCREEN_W, NV_USB_SCREEN_H),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void) instance;
    return desc_hid_report;
}
#endif

enum {
    STR_INDEX_VENDOR = 4,
#if CFG_TUD_HID
    STR_INDEX_HID,
#endif
};

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN * CFG_TUD_HID + \
                             TUD_VENDOR_DESC_LEN * CFG_TUD_VENDOR)

uint8_t const desc_fs_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
#if CFG_TUD_VENDOR
    // Interface number, string index, EP Out & IN address, EP size
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STR_INDEX_VENDOR, EPNUM_VENDOR, 0x80 | EPNUM_VENDOR, CFG_TUD_VENDOR_EPSIZE),
#endif
#if CFG_TUD_HID
    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STR_INDEX_HID, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), (0x80 | EPNUM_HID_DATA), CFG_TUD_HID_EP_BUFSIZE, 10),
#endif
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index;
    return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

#define _STRINGIFY(x)   #x
#define STRINGIFY(s)    _STRINGIFY(s)

// Parsed by the IDD driver: resolution, encode quality, fps cap, max frame bytes.
#define VENDOR_STR \
        CONFIG_IDF_TARGET \
        "udisp0_" \
        "R" \
        STRINGIFY(NV_USB_SCREEN_W) \
        "x" \
        STRINGIFY(NV_USB_SCREEN_H) \
        "_" \
        "Ejpg" \
        STRINGIFY(NV_USB_JPEG_QUALITY) \
        "_Fps" \
        STRINGIFY(NV_USB_MAX_FPS) \
        "_Bl" \
        STRINGIFY(NV_USB_FRAME_LIMIT_B)

char const *string_desc_arr [] = {
    (const char[]) { 0x09, 0x04 },    // 0: language = English (0x0409)
    USB_MANUFACTURER,                 // 1: Manufacturer
    "NucleoV2 Extend Screen",         // 2: Product
    "NV2-0001",                       // 3: Serial
    VENDOR_STR,                       // 4: Vendor interface (geometry/limits for the driver)
#if CFG_TUD_HID
    "touch",                          // 5: HID interface
#endif
};

static uint16_t _desc_str[64];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void) langid;

    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }

        const char *str = (char *)string_desc_arr[index];

        chr_count = (uint8_t) strlen(str);
        if (chr_count > 63) {
            chr_count = 63;
        }

        // ASCII -> UTF-16
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}
