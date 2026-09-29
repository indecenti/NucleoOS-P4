# esp_driver_cam — vendored from ESP-IDF v5.5.2

This directory shadows the IDF component of the same name (a project component wins over the
IDF one). It is the unmodified v5.5.2 source (`test_apps/` dropped) plus `nucleo-backport.patch`,
which backports two fixes already present on ESP-IDF master in `csi/src/esp_cam_ctlr_csi.c`:

1. `esp_cam_new_csi_ctlr()` sets `csi_fsm = CSI_FSM_INIT` right after claiming the controller.
   In v5.5.2 it was set only on success, so any failure after the claim (on this board: no
   contiguous 4 MB of PSRAM for the driver's backup frame buffer) reached `s_del_csi_ctlr()` with
   state 0, which refuses to run: the controller stayed claimed and the camera could not be opened
   again until reboot ("no available csi controller").
2. `s_del_csi_ctlr()` skips `vQueueDeleteWithCaps()` when the queue was never created.

When the project moves to an ESP-IDF release that contains both fixes, delete this directory.
To re-vendor a newer IDF instead: copy `components/esp_driver_cam` from it, drop `test_apps/`, and
re-apply `nucleo-backport.patch` only if the fixes are still missing.
