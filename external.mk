# Pull in every RNSBox package makefile. Adding a package under package/
# is enough — no registration list to maintain.
include $(sort $(wildcard $(BR2_EXTERNAL_RNSBOX_PATH)/package/*/*.mk))

# dnsmasq: the box has no RTC by default and S45ntpsync steps the clock in
# the background after dnsmasq is already up, so leases must time on
# CLOCK_MONOTONIC — otherwise leases handed out at 1970 expire at the step
# and renewals get NAKed. Mirrors the in-tree dnsmasq.mk tweak the lichee
# series carries; external.mk is included after the package makefiles, so
# this appends to the in-tree package's flags without redefining it.
DNSMASQ_COPTS += -DHAVE_BROKEN_RTC

# uhttpd/libubox (in-tree, pinned OpenWrt commits) predate CMake 4: their
# cmake_minimum_required(<3) is fatal under host-cmake 4.x unless the policy
# floor is relaxed — same fix the lichee series carried in its pinned copies.
UHTTPD_CONF_OPTS += -DCMAKE_POLICY_VERSION_MINIMUM=3.5
LIBUBOX_CONF_OPTS += -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# uhttpd relays CGI output as HTTP/1.1 chunks; the terminating zero chunk (and
# every small chunk of a streaming response) is a separate write that Nagle
# holds until the peer's delayed ACK arrives. That is ~40-50 ms on every CGI
# response and on each Console output chunk. Patch-style BSPs get the same
# patch copied into their package/uhttpd by build.sh (rnsbox_slim_bsp); the
# grep keeps the two paths from applying it twice.
ifneq ($(BR2_EXTERNAL_RNSBOX_PATH),)
RNSBOX_UHTTPD_PATCH_DIR = $(BR2_EXTERNAL_RNSBOX_PATH)/board/common/buildroot-patches/uhttpd
define RNSBOX_UHTTPD_TCP_NODELAY
	if ! grep -q TCP_NODELAY $(@D)/client.c; then \
		$(APPLY_PATCHES) $(@D) $(RNSBOX_UHTTPD_PATCH_DIR) \*.patch; \
	fi
endef
UHTTPD_POST_PATCH_HOOKS += RNSBOX_UHTTPD_TCP_NODELAY
endif
