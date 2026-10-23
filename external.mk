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

# HT-HD01-V2: mainline has no board DTS for this box and its Ralink Kconfig
# has no DTB symbol for a custom board.  Drop the DTS into the kernel tree and
# add the one-line Kconfig/Makefile entries before kconfig runs, so the DTB is
# built into the kernel (the stock U-Boot 1.1.3 cannot pass a DTB).
ifeq ($(call qstrip,$(BR2_LINUX_KERNEL_INTREE_DTS_NAME)),ralink/ht-hd01-v2)
define HT_HD01_V2_KERNEL_DTS
	$(INSTALL) -D -m 0644 \
		$(BR2_EXTERNAL_RNSBOX_PATH)/board/ht-hd01-v2-mainline/ht-hd01-v2.dts \
		$(LINUX_DIR)/arch/mips/boot/dts/ralink/ht-hd01-v2.dts
	grep -q '^config DTB_EKH03V3$$' $(LINUX_DIR)/arch/mips/Kconfig || \
		printf '\nconfig DTB_EKH03V3\n\tbool "Heltec HT-HD01-V2"\n\tdepends on SOC_MT7620\n\tselect BUILTIN_DTB\n' \
			>> $(LINUX_DIR)/arch/mips/Kconfig
	grep -q 'ht-hd01-v2.dtb' $(LINUX_DIR)/arch/mips/boot/dts/ralink/Makefile || \
		printf 'dtb-$$(CONFIG_DTB_EKH03V3)\t+= ht-hd01-v2.dtb\n' \
			>> $(LINUX_DIR)/arch/mips/boot/dts/ralink/Makefile
endef
LINUX_POST_PATCH_HOOKS += HT_HD01_V2_KERNEL_DTS
endif

# uhttpd patches, shared with the patch-style BSPs (build.sh copies them into
# their package/uhttpd; rnsbox_slim_bsp). Each hook checks for its own change
# so an incremental tree only gets the patch it is missing.
#  - TCP_NODELAY: CGI output is relayed as HTTP/1.1 chunks; the terminating
#    zero chunk (and every small console chunk) is a separate write that Nagle
#    holds until the peer's delayed ACK arrives (~40-50 ms each time).
#  - tzset(): musl's strptime("%Z") reads tzname[0] without calling tzset();
#    tzname is NULL until then, so uhttpd crashed on any If-Modified-Since
#    header, which browsers send for cached CSS/JS.
ifneq ($(BR2_EXTERNAL_RNSBOX_PATH),)
RNSBOX_UHTTPD_PATCH_DIR = $(BR2_EXTERNAL_RNSBOX_PATH)/board/common/buildroot-patches/uhttpd
define RNSBOX_UHTTPD_TCP_NODELAY
	if ! grep -q TCP_NODELAY $(@D)/client.c; then \
		$(APPLY_PATCHES) $(@D) $(RNSBOX_UHTTPD_PATCH_DIR) 0003-client-tcp-nodelay.patch; \
	fi
endef
define RNSBOX_UHTTPD_TZSET
	if ! grep -q 'tzset();' $(@D)/file.c; then \
		$(APPLY_PATCHES) $(@D) $(RNSBOX_UHTTPD_PATCH_DIR) 0004-file-tzset-before-strptime.patch; \
	fi
endef
UHTTPD_POST_PATCH_HOOKS += RNSBOX_UHTTPD_TCP_NODELAY RNSBOX_UHTTPD_TZSET
endif
