// nftgen.h — nftables ruleset generator (native port of nftgen.py).
// Single source of truth for /etc/nftables.conf, used two ways:
//   - the portal's apply path (config change), and
//   - the boot path: `rnsbox-portal nftgen --wan eth0,wlan0 --lan br-lan`
//     invoked by /etc/init.d/S60routing.
// The WAN is a set of ifaces (@wan_ifaces): the one uplink wan.conf names, or
// eth0 + wlan0 for interface=auto, where the kernel's route metric decides
// which carries traffic (S60routing's header).
// The LAN is the one iface S30gadget_nic records in /run/lan-iface: the
// bridge br-lan (USB-C link + hotspot as ports, one subnet), or usb0 when the
// bridge couldn't be made. Traffic between bridge ports is switched at L2 and
// never reaches these tables.
// Emits `table ip filter` + `table ip nat` (the router) and, when with_ipv6,
// a drop-policy `table ip6 filter` that only admits the LAN ifaces (IPv6 is
// there solely for Reticulum's AutoInterface link-local traffic on the LAN).
#ifndef RNSBOX_NFTGEN_H
#define RNSBOX_NFTGEN_H

#include <string>
#include <vector>

namespace nftgen {

std::string generate(const std::vector<std::string>& wan_ifaces, const std::vector<std::string>& lan_ifaces,
                     bool with_ipv6 = true);

// CLI: parse --wan a[,b]/--lan[/--no-ipv6] from argv, print the ruleset to stdout.
// Without --lan: the LAN iface from /run/lan-iface (sysinfo::lan_iface() —
// same rule as S60routing read_lan()).
// The ip6 table is emitted only if the kernel has IPv6 (/proc/sys/net/ipv6)
// and --no-ipv6 wasn't given. Returns exit code.
int cli_main(int argc, char** argv);

}  // namespace nftgen
#endif
