#!/bin/bash
# RNSBox post-image: ship the usb.* gadget flag files on the boot FAT (the
# /boot flag-file semantics S10usbdev composes the gadget from), pin our
# kernel cmdline, add the DTB alias + kernel-built overlays the universal
# image needs, then assemble boot.vfat + sdcard.img.
#
# The assembly part is the stock buildroot raspberrypi post-image inlined:
# in an external tree there is no ../post-image.sh to delegate to, and the
# logic (dtb/rpi-firmware globbing + the genimage.cfg.in template) is
# board-generic anyway. The boot FAT is mounted at /boot by fstab, so the
# flags are visible to the init scripts (and editable from any PC).
#
# Universal-image notes (one image for every 64-bit Pi):
#   - every board DTB built by the defconfig lands in BINARIES_DIR and is
#     globbed below; the boot firmware picks the right one per model;
#   - the kernel arm64 build produces the overlays under
#     arch/arm64/boot/dts/overlays (a symlink to the shared arm sources) —
#     they are copied wholesale into rpi-firmware/overlays/ so dtoverlay=
#     lines resolve on every model;
#   - the firmware looks for bcm2710-rpi-3-a-plus.dtb on the 3A+ while the
#     arm64 kernel builds that board as bcm2837-rpi-3-a-plus.dtb — the
#     alias below closes the gap.
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
echo "RNSBox Reticulum router (Raspberry Pi, universal 64-bit)" > "${FW}/usb.product"
echo "02:1d:6b:01:04:02"      > "${FW}/usb.ncm.mac"
echo "rnsbox"                 > "${FW}/hostname"
# usb.disk0 (+ .size) for the read-only clients DVD is added by
# scripts/build-variant.sh for the dvd variant; lite ships NCM-only.

# Our kernel cmdline (no GPIO-serial console — the PL011 belongs to SLIP on
# the Zero 2 W; the Pi 5 debug-UART console rides on ttyAMA10).
cat "${BOARD_DIR}/cmdline.txt" > "${FW}/cmdline.txt"

# Kernel-built DTB overlays (arm64 overlay build via the arm/ dts symlink):
# all *.dtbo plus the overlay_map.dtb/hat_map.dtb model maps and README.
KOVERLAYS=$(echo "${BUILD_DIR}"/linux-*/arch/arm64/boot/dts/overlays)
if [ -d "${KOVERLAYS}" ]; then
	mkdir -p "${FW}/overlays"
	# kernel-built files supersede the two stray dtbos the rpi-firmware
	# repo ships in its own boot/overlays
	cp -f "${KOVERLAYS}"/*.dtbo "${FW}/overlays/" 2>/dev/null || true
	for f in overlay_map.dtb hat_map.dtb README; do
		[ -f "${KOVERLAYS}/${f}" ] && cp -f "${KOVERLAYS}/${f}" "${FW}/overlays/"
	done
else
	echo "WARNING: kernel overlays not found at ${KOVERLAYS}" >&2
fi

# 3A+ DTB alias (firmware name <- arm64 kernel name)
if [ -f "${BINARIES_DIR}/bcm2837-rpi-3-a-plus.dtb" ] && \
   [ ! -f "${BINARIES_DIR}/bcm2710-rpi-3-a-plus.dtb" ]; then
	cp -f "${BINARIES_DIR}/bcm2837-rpi-3-a-plus.dtb" \
	      "${BINARIES_DIR}/bcm2710-rpi-3-a-plus.dtb"
fi

# NOTE: the A/B whole-OS updater (dual rootfs slots + slot-selector
# initramfs + rnsbox-abselect on the boot FAT) is DISABLED for now — the
# image ships the plain single-rootfs layout like every other board. The
# machinery stays in the tree (initramfs/, package/rnsbox-update); to
# re-enable, restore the initramfs staging here, the `initramfs` line in
# config.txt and the two-slot genimage template (see git history).
# The boot.vfat file set globs everything in rpi-firmware/, so an output
# dir from an A/B-era build would keep shipping the stale selector —
# actively remove its artifacts.
rm -f "${FW}/initramfs.gz" "${FW}/initramfs.cpio" "${FW}/rnsbox-abselect" \
      "${FW}/ab_slot" "${FW}/ab_tries"

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
