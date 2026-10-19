#!/bin/bash
# RNSBox mainline sg2002 (LicheeRV Nano) image assembly:
#   boot.sd    = u-boot FIT: mainline Image.gz @ 0x80200000 + mainline DTB,
#                config-sg2002_licheervnano_sd (the name the older vendor
#                u-boot boots; the current one boots the default config,
#                which is this one);
#   boot.vfat  = vendor fip.bin + boot.sd + usb.* gadget flag files;
#   sdcard.img = boot FAT + rootfs.ext4 (root=/dev/mmcblk0p2 comes baked into
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

# fip.bin — provenance-pinned vendor blob (README.fip.md). RNSBOX_FIP_SRC
# overrides the source for a newer blob (then refresh fip.bin.sha256).
FIP_SRC="${RNSBOX_FIP_SRC:-${BOARD_DIR}/fip.bin}"
echo "$(cut -d' ' -f1 "${BOARD_DIR}/fip.bin.sha256")  ${FIP_SRC}" | sha256sum -c -
cp -f "${FIP_SRC}" "${BINARIES_DIR}/fip.bin"

# FIT payload: gzip the Image (u-boot gunzips to 0x80200000) + the DTB
# buildroot installed next to it (BR2_LINUX_KERNEL_INTREE_DTS_NAME).
gzip -9 -c "${BINARIES_DIR}/Image" > "${BINARIES_DIR}/Image.gz"
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
echo "RNSBox Reticulum router (licheerv nano, mainline)" > "${BINARIES_DIR}/usb.product"
echo "02:1d:6b:01:05:02"      > "${BINARIES_DIR}/usb.ncm.mac"
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
TAGGED="${BINARIES_DIR}/rnsbox-${RNSBOX_BOARD:-sg2002-licheervnano-mainline}-${VARIANT}.img"
cp -f "$NEW" "$TAGGED"
echo "=================== IMAGE OK [${VARIANT}] $(date) ==================="
