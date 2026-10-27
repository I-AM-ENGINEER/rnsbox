#!/bin/bash
# RNSBox SG2002 ARM64 (LicheeRV Nano B) image assembly:
#   boot.sd    = u-boot FIT: arm64 Image @ 0x80200000 + DTB,
#                config name "sg2002_licheea53nano_sd" (the vendor u-boot
#                blob is the SG2000 ARM64 one and names its boot config
#                after its board dir — the LicheeRV Nano A53 build);
#   boot.vfat  = vendor ARM64 fip.bin + boot.sd + usb.* gadget flag files;
#   sdcard.img = boot FAT + rootfs.ext4 (root=/dev/mmcblk0p2 is baked into
#                the vendor u-boot bootargs).
# Same flow as the sibling RISC-V Duo Module 01 image (board/sg2002/)
# and the Duo S ARM64 one (board/sg2000-duo-s-arm64/).
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
DTB="${BINARIES_DIR}/sg2002-licheerv-nano-b.dtb"
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
echo "RNSBox Reticulum router (licheerv-nano arm64)" > "${BINARIES_DIR}/usb.product"
echo "02:1d:6b:01:05:05"      > "${BINARIES_DIR}/usb.ncm.mac"
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

# Tag the image the way every board does (build.sh and the workflow look
# for rnsbox-$BOARD.img). RNSBOX_BOARD is exported by build.sh.
NEW="${BINARIES_DIR}/sdcard.img"
TAGGED="${BINARIES_DIR}/rnsbox-${RNSBOX_BOARD:-sg2002-licheerv-nano-arm64}.img"
cp -f "$NEW" "$TAGGED"
echo "=================== IMAGE OK $(date) ==================="
