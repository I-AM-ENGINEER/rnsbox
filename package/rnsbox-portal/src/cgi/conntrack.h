// conntrack.h — `rnsbox-portal ctflush`: delete the IPv4 conntrack entries of
// flows masqueraded out a WAN iface, over netlink (ctnetlink; no
// conntrack-tools in the image), so they don't linger with the previous
// uplink's address after WAN=auto switched between eth0 and the WiFi client
// (S60routing uplink, run by S46wanwatch).
#ifndef RNSBOX_CONNTRACK_H
#define RNSBOX_CONNTRACK_H

namespace conntrack {

// ct mark bit nftgen's nat postrouting sets on every flow it masquerades out
// a WAN iface (the first packet of a flow is the only one nat sees, so the
// mark stays for the flow's life). Nothing else in the ruleset uses ct marks.
// Entries without it — LAN <-> box, the HaLow modem over sl0, inbound to the
// box, port forwards — are left alone by flush().
constexpr unsigned WAN_NAT_MARK = 0x1;

// Returns the process exit code (0 = flushed); errors go to stderr.
int flush();

}  // namespace conntrack
#endif
