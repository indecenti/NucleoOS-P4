// Self-signed https for NucleoCast (see ss_tls.cpp).
#pragma once
#include "ss_net.h"

bool ss_tls_init(void);                     // load/generate the certificate; idempotent
bool ss_tls_ready(void);
bool ss_tls_wrap(SsConn &c, int timeout_ms); // server handshake on an accepted socket
