################################################################################
#
# rnsbox-bt-firmware
#
################################################################################

# Broadcom Bluetooth patches (HCD files) the RNSBox Raspberry Pi image needs
# to bring up the onboard BT controller of every Pi that has one:
#
#   BCM43430A1.hcd  Zero W, Zero 2 W, 3B, CM3(+), 3A+ (CYW43430 family)
#   BCM4345C0.hcd   3B+ (CYW43455)
#   BCM4345C5.hcd   4B/400/CM4/CM4S, 5 (CYW43455C5 variant)
#
# The kernel's hci_uart/btbcm (serdev) and btusb drivers request them from
# /lib/firmware/brcm/ by chip name once the controller is probed; without the
# file the adapter never appears (dmesg: "bcm: failed to load firmware" /
# HCI timeouts). Pi 5's BT is handled by the same serdev path.
#
# Pulled from the Raspberry Pi distro firmware repo (same blobs Raspberry Pi
# OS ships in its bluez-firmware package), pinned to a commit.
RNSBOX_BT_FIRMWARE_VERSION = 6851250bf9d51ff50e3c5b2cf2111e2419d4335b
RNSBOX_BT_FIRMWARE_SITE = https://raw.githubusercontent.com/RPi-Distro/bluez-firmware/$(RNSBOX_BT_FIRMWARE_VERSION)/broadcom
RNSBOX_BT_FIRMWARE_LICENSE = PROPRIETARY

RNSBOX_BT_FIRMWARE_FILES = BCM43430A1.hcd BCM4345C0.hcd BCM4345C5.hcd
# One file is the primary source, the rest come in as extra downloads; all
# three are sha256-checked via rnsbox-bt-firmware.hash.
RNSBOX_BT_FIRMWARE_SOURCE = BCM43430A1.hcd
RNSBOX_BT_FIRMWARE_EXTRA_DOWNLOADS = BCM4345C0.hcd BCM4345C5.hcd

# Raw firmware blobs, not archives: neutralize buildroot's extract step (its
# default would pipe the .hcd through tar) - install reads the blobs
# straight from the dl cache.
define RNSBOX_BT_FIRMWARE_EXTRACT_CMDS
	$(Q)mkdir -p $(@D)
endef

define RNSBOX_BT_FIRMWARE_INSTALL_TARGET_CMDS
	$(foreach f,$(RNSBOX_BT_FIRMWARE_FILES),\
		$(INSTALL) -D -m 0644 $(RNSBOX_BT_FIRMWARE_DL_DIR)/$(f) \
			$(TARGET_DIR)/lib/firmware/brcm/$(f)$(sep))
endef

$(eval $(generic-package))
