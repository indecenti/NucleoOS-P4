// apps_internal — private glue between the app files and nv_apps.cpp.
// Each app file exposes ONE register function that pushes its static NvApp descriptor
// into the SystemUI registry. Order of calls == launcher order.
#pragma once

void settings_app_register(void);
void anima_app_register(void);
void files_app_register(void);
void diagnostics_app_register(void);
void calculator_app_register(void);
void gallery_app_register(void);
void notes_app_register(void);
void apps_app_register(void);
void secondscreen_app_register(void);
void tasks_app_register(void);
void camera_app_register(void);
void terminal_app_register(void);
void music_app_register(void);
void video_app_register(void);
void recorder_app_register(void);
void sysmon_app_register(void);
void apps_register_wasm(void);   // register a launcher tile per installed WASM app (after seed)
