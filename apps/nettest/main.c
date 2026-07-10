// Net Test — smoke-test the ABI v5 UDP surface end-to-end. Broadcasts a beacon on :9999 and echoes
// any datagram it receives (with the sender). Run via the web console (/api/app/run?id=nettest) and,
// from a PC on the same LAN, listen for "NKPING" on UDP 9999 (TX check) and send a packet to the
// device's port 9999 (RX check).
#include "nucleo_sdk.h"

NV_EXPORT("run")
void run(void) {
    nv_printf("net_ip token = %x", nv_net_ip());
    if (nv_net_open(9999) != 0) { nv_print("net_open(9999) FAILED"); return; }
    nv_print("net_open(9999) OK");

    char buf[256];
    for (int i = 0; i < 100; i++) {          // ~10 s
        int s = nv_net_bcast(9999, "NKPING", 6);
        if (i == 0) nv_printf("net_bcast ret = %d", s);
        int n;
        while ((n = nv_net_recv(buf, sizeof buf - 1)) > 0) {
            if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
            buf[n] = 0;
            nv_printf("rx %d bytes from %x:%d = %s", n, nv_net_from_ip(), nv_net_from_port(), buf);
        }
        nv_sleep_ms(100);
    }
    nv_net_close();
    nv_print("net test done");
}
