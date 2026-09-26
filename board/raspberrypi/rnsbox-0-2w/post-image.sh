#!/bin/bash
# RNSBox post-image: ship the usb.* gadget flag files on the boot FAT (the
# /boot flag-file semantics S10usbdev composes the gadget from), pin our
# kernel cmdline, then assemble boot.vfat + sdcard.img.
#
# The assembly part is the stock buildroot raspberrypi post-image inlined:
# in an external tree there is no ../post-image.sh to delegate to, and the
# logic (dtb/rpi-firmware globbing + the genimage.cfg.in template) is
# board-generic anyway. The boot FAT is mounted at /boot by fstab, so the
# flags are visible to the init scripts (and editable from any PC).
set -e

BOARD_DIR="$(dirname "$0")"
FW="${BINARIES_DIR}/rpi-firmware"
GENIMAGE_CFG="${BINARIES_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# CDC-NCM gadget identity (read at boot by S10usbdev). Windows caches the
# MS-OS descriptor by (VID, PID, bcdDevice) — bump bcdDevice whenever the
# gadget topology changes.
: > "${FW}/usb.dev"
: > "${FW}/usb.ncm"
echo 0x1d6b                   > "${FW}/usb.idVendor"
echo 0x0105                   > "${FW}/usb.idProduct"
echo 0x0105                   > "${FW}/usb.bcdDevice"
echo "RNSBox"                 > "${FW}/usb.manufacturer"
echo "RNSBox Reticulum router (Pi Zero 2 W)" > "${FW}/usb.product"
echo "02:1d:6b:01:04:02"      > "${FW}/usb.ncm.mac"
echo "rnsbox"                 > "${FW}/hostname"
# usb.disk0 (+ .size) for the read-only clients DVD is added by
# scripts/build-variant.sh for the dvd variant; lite ships NCM-only.

# Our kernel cmdline (no serial console — the PL011 belongs to SLIP).
cat "${BOARD_DIR}/cmdline.txt" > "${FW}/cmdline.txt"

# ---- stock raspberrypi image assembly (inlined) ----
FILES=()
for i in "${BINARIES_DIR}"/*.dtb "${BINARIES_DIR}"/rpi-firmware/*; do
	FILES+=( "${i#${BINARIES_DIR}/}" )
done
KERNEL=$(sed -n 's/^kernel=//p' "${BINARIES_DIR}/rpi-firmware/config.txt")
FILES+=( "${KERNEL}" )
# NB: the printf emits LITERAL \t and \n escapes on one line (double
# backslashes) — a real newline would terminate sed's s||| command, and
# genimage parses the escapes itself.
BOOT_FILES=$(printf '\\t\\t\\t"%s",\\n' "${FILES[@]}")
sed "s|#BOOT_FILES#|${BOOT_FILES}|" "${BOARD_DIR}/genimage.cfg.in" \
	> "${GENIMAGE_CFG}"

# Pass an empty rootpath: genimage would make a full copy of the rootpath
# under its tmp dir; we don't rely on it to build the rootfs image, only to
# insert the pre-built one into the disk image.
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"

rm -rf "${GENIMAGE_TMP}"

genimage \
	--rootpath "${ROOTPATH_TMP}"   \
	--tmppath "${GENIMAGE_TMP}"    \
	--inputpath "${BINARIES_DIR}"  \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"

rm -rf "${ROOTPATH_TMP}"
trap - EXIT
