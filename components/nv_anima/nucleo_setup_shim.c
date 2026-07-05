#include "nucleo_setup.h"

#include <string.h>

#include "nv_wifi.h"

static char s_ip[16];

const char *nucleo_setup_ip(void) {
    char ip[16] = "";
    if (nv_wifi_get_connected(NULL, 0, ip, sizeof ip, NULL) && ip[0]) {
        strncpy(s_ip, ip, sizeof s_ip - 1);
        s_ip[sizeof s_ip - 1] = '\0';
        return s_ip;
    }
    return "";
}
