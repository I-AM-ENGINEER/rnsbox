// rnstatusd_client.h — fetch the rnstatus JSON from the resident helper.
//
// Starting rnstatus is expensive (a full RNS import on the C906); the helper
// pays that once and answers over /run/rnsbox-rnstatusd.sock. This client
// starts the helper on demand and waits for its first answer. Returns false
// when the helper is unavailable (low-RAM board, no python3, rnsd down), in
// which case the caller falls back to the cached CLI path.
#ifndef RNSBOX_RNSTATUSD_CLIENT_H
#define RNSBOX_RNSTATUSD_CLIENT_H

#include <string>

namespace rnstatusd {

// Raw `rnstatus -a -j` JSON. `wait_ms` bounds the whole operation, including
// starting the helper and its RNS import.
bool fetch_raw(std::string& out, int wait_ms = 25000);

}  // namespace rnstatusd

#endif
