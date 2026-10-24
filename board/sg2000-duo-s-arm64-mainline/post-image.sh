#!/bin/bash
# RNSBox mainline SG2000 ARM64 (Milk-V Duo S) image assembly:
#   boot.sd    = u-boot FIT: mainline arm64 Image @ 0x80200000 + DTB,
#                default/named config "config-sg2000_duo01evb_arm64_sd"
#                (the vendor u-boot blob is the SG2000 ARM64 one and names
#                its boot config after its board dir);
#   boot.vfat  = vendor ARM64 fip.bin + boot.sd + usb.* gadget flag files;
#   sdcard.img = boot FAT + rootfs.ext4 (root=/dev/mmcblk0p2 is baked into
#                the vendor u-boot bootargs).
# Address map and boot-chain facts: board/sg2000-mainline/README.fip.md and
# board/milkv-duo/README.fip.md (the sibling CV1800B flow this one copies).
set -e

BOARD_DIR="$(dirname "$0")"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
MKIMAGE="${HOST_DIR}/bin/mkimage"
# mkimage shells out to dtc for the ITS; make sure the buildroot host
# tools dir (host-dtc) is on its PATH
export PATH="${HOST_DIR}/bin:${PATH}"
GENIMAGE="${HOST_DIR}/bin/genimage"

# fip.bin — provenance-pinned vendor ARM64 blob. RNSBOX_FIP_SRC overrides
# the source for a newer blob (then refresh fip.bin.sha256).
FIP_SRC="${RNSBOX_FIP_SRC:-${BOARD_DIR}/fip.bin}"
echo "$(cut -d' ' -f1 "${BOARD_DIR}/fip.bin.sha256")  ${FIP_SRC}" | sha256sum -c -
cp -f "${FIP_SRC}" "${BINARIES_DIR}/fip.bin"

# FIT payload: the uncompressed arm64 Image + the DTB Buildroot installed
# next to it (BR2_LINUX_KERNEL_INTREE_DTS_NAME).
DTB="${BINARIES_DIR}/sg2000-milkv-duo-s.dtb"
[ -f "${DTB}" ] || { echo "FATAL: ${DTB} missing" >&2; exit 1; }
cp -f "${BOARD_DIR}/boot.its" "${BINARIES_DIR}/boot.its"
( cd "${BINARIES_DIR}" && "${MKIMAGE}" -f boot.its boot.sd )

# usb.* gadget flags — same /boot flag-file semantics as every RNSBox image
# (S10usbdev composes the configfs CDC-NCM gadget from them). Windows caches
# the MS-OS descriptor by (VID, PID, bcdDevice): bump bcdDevice whenever the
# gadget topology changes.
: > "${BINARIES_DIR}/usb.dev"
: > "${BINARIES_DIR}/usb.ncm"
echo 0x1d6b                   > "${BINARIES_DIR}/usb.idVendor"
echo 0x0105                   > "${BINARIES_DIR}/usb.idProduct"
echo 0x0105                   > "${BINARIES_DIR}/usb.bcdDevice"
echo "RNSBox"                 > "${BINARIES_DIR}/usb.manufacturer"
echo "RNSBox Reticulum router (milkv duo-s arm64, mainline)" > "${BINARIES_DIR}/usb.product"
echo "02:1d:6b:01:05:03"      > "${BINARIES_DIR}/usb.ncm.mac"
echo "rnsbox"                 > "${BINARIES_DIR}/hostname"
date -u +%Y-%m-%dT%H:%M:%SZ   > "${BINARIES_DIR}/ver"

# Empty rootpath: genimage would copy the whole target tree under its tmp
# dir; the rootfs.ext4 is already built, genimage only embeds it.
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"
rm -rf "${GENIMAGE_TMP}"
"${GENIMAGE}" \
        --rootpath "${ROOTPATH_TMP}"   \
        --tmppath "${GENIMAGE_TMP}"    \
        --inputpath "${BINARIES_DIR}"  \
        --outputpath "${BINARIES_DIR}" \
        --config "${BOARD_DIR}/genimage.cfg"
rm -rf "${ROOTPATH_TMP}"
trap - EXIT

# Tag the image the house way (the pc-x86_64 post-image does the same).
# RNSBOX_BOARD / RNSBOX_VARIANT are exported by build.sh.
VARIANT="${RNSBOX_VARIANT:-lite}"
NEW="${BINARIES_DIR}/sdcard.img"
SUFFIX=""
if [ "$VARIANT" != lite ]; then SUFFIX="-$VARIANT"; fi
TAGGED="${BINARIES_DIR}/rnsbox-${RNSBOX_BOARD:-sg2000-duo-s-arm64}${SUFFIX}.img"
cp -f "$NEW" "$TAGGED"
echo "=================== IMAGE OK [${VARIANT}] $(date) ==================="
