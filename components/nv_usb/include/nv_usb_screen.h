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
#define NV_USB_TOUCH_MAX      5       // GT911 reports up to 5 fingers
