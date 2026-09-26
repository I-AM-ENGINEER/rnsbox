// conntrack.cpp — see conntrack.h. Kept apart from sysinfo.h: the musl
// kernel headers pulled in here declare `struct sysinfo`, which clashes with
// the sysinfo namespace.
#include "conntrack.h"
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace conntrack {

int flush() {
    // Delete the IPv4 entries carrying WAN_NAT_MARK: IPCTNL_MSG_CT_DELETE
    // with no tuple is a flush, a nonzero nfgenmsg version limits it to
    // nfgen_family, and CTA_MARK/CTA_MARK_MASK to entries whose
    // (mark & mask) == mark (5.10 ctnetlink_del_conntrack ->
    // ctnetlink_filter_match). What was mid-flow is picked up again on its
    // next packet and masqueraded to the current uplink (TCP: the peer
    // resets it and the client reconnects; UDP such as WireGuard just
    // continues from the new address).
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_NETFILTER);
    if (fd < 0) { perror("ctflush: socket"); return 1; }
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct {
        struct nlmsghdr nh;
        struct nfgenmsg nf;
        struct nlattr mark_hdr;
        uint32_t mark;            // big-endian
        struct nlattr mask_hdr;
        uint32_t mask;            // big-endian
    } req;
    static_assert(sizeof req == NLMSG_LENGTH(sizeof(struct nfgenmsg) + 2 * (NLA_HDRLEN + 4)),
                  "unpadded netlink request");
    memset(&req, 0, sizeof req);
    req.nh.nlmsg_len = sizeof req;
    req.nh.nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_DELETE;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nh.nlmsg_seq = 1;
    req.nf.nfgen_family = AF_INET;
    req.nf.version = 1;
    req.mark_hdr.nla_len = NLA_HDRLEN + sizeof req.mark;
    req.mark_hdr.nla_type = CTA_MARK;
    req.mark = htonl(WAN_NAT_MARK);
    req.mask_hdr.nla_len = NLA_HDRLEN + sizeof req.mask;
    req.mask_hdr.nla_type = CTA_MARK_MASK;
    req.mask = htonl(WAN_NAT_MARK);
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    if (sendto(fd, &req, req.nh.nlmsg_len, 0, (struct sockaddr*)&sa, sizeof sa) < 0) {
        perror("ctflush: send");
        close(fd);
        return 1;
    }
    char buf[512];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    close(fd);
    const struct nlmsghdr* h = (const struct nlmsghdr*)buf;
    if (n < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr)) || h->nlmsg_type != NLMSG_ERROR) {
        fprintf(stderr, "ctflush: no acknowledgement from the kernel\n");
        return 1;
    }
    int err = ((const struct nlmsgerr*)NLMSG_DATA(h))->error;
    if (err) {
        fprintf(stderr, "ctflush: %s\n", strerror(-err));
        return 1;
    }
    return 0;
}

}  // namespace conntrack
