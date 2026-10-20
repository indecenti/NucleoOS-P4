// Registers the system's background services with nv_service_mgr (see system_services.cpp).
#pragma once

// Once, at the end of boot. `safe_mode`: the services safe mode leaves off are not listed;
// `usb_host`: the OTG port runs as a USB host (else as the PC second-screen device).
void system_services_register(bool safe_mode, bool usb_host);
