// nv_usb — extended-screen geometry/limits advertised to the Windows IDD driver
// through the vendor interface string descriptor ("R{W}x{H}_Ejpg{Q}_Fps{F}_Bl{limit}").
// 1024x576 (16:9): JPEG-MCU aligned (576 = 36*16); the panel is 1024x600, so frames
// are drawn with a 12 px letterbox top/bottom. Same geometry Espressif ships for the
// P4 Function-EV board (also a 1024x600 panel).
#pragma once

#define NV_USB_SCREEN_W       1024
#define NV_USB_SCREEN_H       576
#define NV_USB_JPEG_QUALITY   6       // 1..10, PC-side encode quality hint
#define NV_USB_MAX_FPS        60
#define NV_USB_FRAME_LIMIT_B  300000  // max JPEG bytes per frame accepted from the PC
// The same limit as advertised to the driver ("_Bl<n>"): the Windows IDD driver multiplies it by
// 1024 (KB). Advertising bytes (Bl300000 = ~293 MB to the driver) meant it never lowered the JPEG
// quality to fit, and every busy-desktop frame over 300 KB was dropped here. Must be a literal
// (stringified into the descriptor) and <= NV_USB_FRAME_LIMIT_B / 1024.
#define NV_USB_FRAME_LIMIT_KB 292
#define NV_USB_TOUCH_MAX      5       // GT911 reports up to 5 fingers
