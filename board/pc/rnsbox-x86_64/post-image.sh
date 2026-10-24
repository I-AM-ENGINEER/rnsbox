#!/bin/bash
# RNSBox PC/VM post-image: assemble the BIOS disk (MBR grub + one ext4 root)
# with genimage and tag it images/rnsbox-pc-x86_64.img (no clients-DVD gadget
# LUN — the VM has no USB gadget to expose it on).
set -e

BOARD_DIR="$(dirname "$0")"
GENIMAGE_CFG="${BOARD_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

rm -rf "${GENIMAGE_TMP}"

# Pass an empty rootpath: genimage would make a full copy of the rootpath
# under its tmp dir; we don't rely on it to build the rootfs image, only to
# insert the pre-built one into the disk image.
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"

genimage \
	--rootpath "${ROOTPATH_TMP}"   \
	--tmppath "${GENIMAGE_TMP}"    \
	--inputpath "${BINARIES_DIR}"  \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"

rm -rf "${ROOTPATH_TMP}"
trap - EXIT


NEW="${BINARIES_DIR}/disk.img"
TAGGED="${BINARIES_DIR}/rnsbox-${RNSBOX_BOARD:-pc-x86_64}.img"
cp -f "$NEW" "$TAGGED"
echo "=================== IMAGE OK $(date) ==================="
ls -lh "$TAGGED" | awk '{print "  "$5"  "$NF}'
echo "DONE-SENTINEL-OK"
