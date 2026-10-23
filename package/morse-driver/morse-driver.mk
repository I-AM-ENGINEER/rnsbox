################################################################################
#
# morse-driver — Morse Micro MM6108 HaLow (SDIO)
#
################################################################################

MORSE_DRIVER_VERSION = mm6108-2.1.1
MORSE_DRIVER_SITE = https://github.com/MorseMicro/morse_driver.git
MORSE_DRIVER_SITE_METHOD = git
MORSE_DRIVER_GIT_SUBMODULES = YES
MORSE_DRIVER_LICENSE = GPL-2.0
MORSE_DRIVER_LICENSE_FILES = LICENSE
MORSE_DRIVER_DEPENDENCIES = linux

# The driver's own Makefile is Kbuild-compatible: morse.ko + dot11ah.ko.
# CONFIG_WLAN_VENDOR_MORSE=m is a make variable here, not a kernel symbol.
MORSE_DRIVER_MODULE_MAKE_OPTS = \
	CONFIG_WLAN_VENDOR_MORSE=m \
	CONFIG_MORSE_SDIO=y \
	CONFIG_MORSE_VENDOR_COMMAND=y \
	CONFIG_MORSE_USER_ACCESS=y \
	CONFIG_MORSE_DEBUG=n \
	DEBUG=n \
	KCFLAGS="-Wno-error -Wno-missing-prototypes -Wno-incompatible-pointer-types"

# The mm6108-2.1.1 tag predates Linux 7.3.  Two API/warning fixes are needed:
#  - cfg80211_ops.mgmt_tx() now passes the cookie by value (u64) instead of
#    u64*; writing through the old pointer would corrupt the caller.
#  - the kernel's missing-prototypes warning is fatal under the driver's own
#    -Werror; KCFLAGS above relaxes that without changing behaviour.
define MORSE_DRIVER_FIX_LINUX_7_3
	$(SED) 's/-Werror//g' $(@D)/Makefile
	$(SED) 's/^\([[:space:]]*u64\)[[:space:]]*\*cookie)/\1 cookie)/' $(@D)/wiphy.c
	$(SED) 's/\*cookie = le32_to_cpu(hdr->tx_info.pkt_id);/(void)cookie;/' $(@D)/wiphy.c
endef
MORSE_DRIVER_POST_PATCH_HOOKS += MORSE_DRIVER_FIX_LINUX_7_3

$(eval $(kernel-module))
$(eval $(generic-package))
