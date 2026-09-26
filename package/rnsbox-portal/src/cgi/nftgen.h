// nftgen.h — nftables ruleset generator (native port of nftgen.py).
// Single source of truth for /etc/nftables.conf, used two ways:
//   - the portal's apply path (config change), and
//   - the boot path: `rnsbox-portal nftgen --wan wlan0 --lan usb0[,wlan1]`
//     invoked by /etc/init.d/S60routing.
#ifndef RNSBOX_NFTGEN_H
#define RNSBOX_NFTGEN_H

#include <string>
#include <vector>

namespace nftgen {

std::string generate(const std::string& wan_iface, const std::vector<std::string>& lan_ifaces);

// CLI: parse --wan/--lan from argv, print the ruleset to stdout. Returns exit code.
int cli_main(int argc, char** argv);

}  // namespace nftgen
#endif
