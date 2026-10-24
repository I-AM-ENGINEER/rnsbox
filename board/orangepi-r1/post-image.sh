#!/bin/bash
# RNSBox post-image for the Orange Pi R1.
#
#  1. ship the usb.* gadget flag files on the boot FAT (the /boot flag-file
#     semantics S10usbdev composes the gadget from),
#  2. assemble boot.vfat + the GPT SD image with genimage,
#  3. tag the result as the board/variant image build.sh reports.
#
# The kernel/DTB/boot.scr themselves are produced by Buildroot (the boot
# script source is wired through BR2_PACKAGE_HOST_UBOOT_TOOLS_BOOT_SCRIPT_*).
set -e

BOARD_DIR="$(dirname "$0")"
: "${BINARIES_DIR:?}"
: "${TARGET_DIR:?}"
: "${BUILD_DIR:?}"

# CDC-NCM gadget identity (read at boot by S10usbdev). Windows caches the
# MS-OS descriptor by (VID, PID, bcdDevice) — bump bcdDevice whenever the
# gadget topology changes.
: > "${BINARIES_DIR}/usb.dev"
: > "${BINARIES_DIR}/usb.ncm"
echo 0x1d6b                   > "${BINARIES_DIR}/usb.idVendor"
echo 0x0105                   > "${BINARIES_DIR}/usb.idProduct"
echo 0x0105                   > "${BINARIES_DIR}/usb.bcdDevice"
echo "RNSBox"                 > "${BINARIES_DIR}/usb.manufacturer"
echo "RNSBox Reticulum router (Orange Pi R1)" > "${BINARIES_DIR}/usb.product"
echo "02:1d:6b:01:04:03"      > "${BINARIES_DIR}/usb.ncm.mac"
echo "rnsbox"                 > "${BINARIES_DIR}/hostname"
# One NCM gadget; no clients disc.

# boot.cmd -> boot.scr (mkimage comes from host-uboot-tools, enabled in the
# defconfig). Buildroot already generated it through the *_BOOT_SCRIPT
# symbols; generate again here only if the file is missing.
if [ ! -f "${BINARIES_DIR}/boot.scr" ]; then
	"${HOST_DIR}/bin/mkimage" -A arm -O linux -T script -C none \
		-n "RNSBox Orange Pi R1 boot" \
		-d "${BOARD_DIR}/boot.cmd" "${BINARIES_DIR}/boot.scr"
fi

GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
rm -rf "${GENIMAGE_TMP}"

genimage \
	--rootpath "${TARGET_DIR}" \
	--tmppath "${GENIMAGE_TMP}" \
	--inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" \
	--config "${BOARD_DIR}/genimage.cfg"

# build.sh exports RNSBOX_BOARD for the external-tree boards.

TAGGED="${BINARIES_DIR}/rnsbox-${RNSBOX_BOARD:-orangepi-r1}.img"
cp -f "${BINARIES_DIR}/sdcard.img" "${TAGGED}"
echo "post-image: ${TAGGED} ($(stat -c%s "${TAGGED}") bytes)"
