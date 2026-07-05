// nv_usb TinyUSB configuration — vendor bulk (JPEG frames) + HID touchscreen composite.
// P4 runs the OTG-HS controller (RHPort1) at high speed; no UAC (audio handled by nv_audio).
// (Adapted from esp-iot-solution usb_extend_screen, MIT.)
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// Board / port configuration
//--------------------------------------------------------------------

#if CONFIG_IDF_TARGET_ESP32P4
#define CFG_TUSB_RHPORT1_MODE   (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define NV_USB_HS               1
#else
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define NV_USB_HS               0
#endif

//--------------------------------------------------------------------
// Common configuration
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_FREERTOS
#endif

#ifndef ESP_PLATFORM
#define ESP_PLATFORM 1
#endif

// Espressif IDF requires "freertos/" prefix in include path
#if TU_CHECK_MCU(OPT_MCU_ESP32S2, OPT_MCU_ESP32S3, OPT_MCU_ESP32P4)
#define CFG_TUSB_OS_INC_PATH    freertos/
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG          1
#endif

#define CFG_TUD_ENABLED         1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// Device configuration
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE  64
#endif

// VID/PID matched by the Windows IDD driver INF (composite vendor+HID = 0x2986;
// a display-only device would use 0x2987). Do not change without a driver rebuild.
#define USB_VID                 0x303A
#define USB_PID                 0x2986
#define USB_MANUFACTURER        "NucleoV2"

//------------- Classes -------------//
#define CFG_TUD_VENDOR              1
#define VENDOR_BUF_SIZE             (NV_USB_HS ? 512 : 64)
#define CFG_TUD_VENDOR_RX_BUFSIZE   (VENDOR_BUF_SIZE * 10)
#define CFG_TUD_VENDOR_TX_BUFSIZE   VENDOR_BUF_SIZE
#ifndef CFG_TUD_VENDOR_EPSIZE
#define CFG_TUD_VENDOR_EPSIZE       VENDOR_BUF_SIZE
#endif

#define CFG_TUD_HID                 1
#define CFG_TUD_HID_EP_BUFSIZE      (NV_USB_HS ? 512 : 64)

#ifdef __cplusplus
}
#endif
