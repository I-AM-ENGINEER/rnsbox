// modem.h — the small HTTP client the portal uses to configure the HaLow
// modem directly (the modem's own /api/* JSON endpoints), plus the address
// the modem should send telemetry to.
#ifndef RNSBOX_MODEM_H
#define RNSBOX_MODEM_H

#include "store.h"
#include <string>

namespace modem {

// POST one JSON object to http://<peer>/api/<endpoint>. Short timeouts; false
// on any connect/HTTP failure. `resp` keeps the raw response for diagnostics.
bool post(const std::string& peer, const std::string& endpoint,
          const std::string& json, std::string& resp);

// Local address the modem should use as its MQTT telemetry destination: the
// SLIP local IP on a UART link, or the routed source address on an IP link.
std::string local_ip_towards(const store::SlipConfig& sl);

}  // namespace modem

#endif
