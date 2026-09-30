NucleoOS P4 - Second Screen over USB: Windows driver
===================================================

Status: experimental (see docs/STATUS.md).

The board can act as a USB monitor for a Windows 10/11 PC through a signed Indirect Display
Driver published by Espressif (esp-iot-solution, usb_extend_screen). It is not redistributed in
this repository; download it from Espressif and copy it into this folder on the SD card:

  https://dl.espressif.com/AE/esp-iot-solution/xfz1986_usb_graphic_250224_rc_sign.exe

Source of the driver: github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display

Install:
1. Connect the board's HIGH-SPEED USB-C port to the PC.
2. Run the .exe and press Install (it is signed: no test mode needed).
3. Windows detects a new 1024x576 monitor: arrange it in Settings > System > Display.
4. Open the Second Screen app on the board.

USB device: VID 303A, PID 2986 ("NucleoV2 Extend Screen").
If touch drives the wrong screen: Control Panel > Tablet PC Settings > Setup.
