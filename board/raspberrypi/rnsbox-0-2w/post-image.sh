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

# ---- RNSBox A/B updater: mini initramfs (slot selector) + abselect staging ----
# /init picks rootfs_A/B from the shared boot FAT state (ab_slot/ab_tries;
# decision logic sourced from /boot/rnsbox-abselect, else the initramfs copy,
# else inline — see initramfs/init). Built from the TARGET busybox + the
# kernel tree's gen_init_cpio (creates /dev/console without root). Any missing
# ingredient (no gen_init_cpio, no busybox) => no initramfs is shipped and the
# firmware boots the old single-slot way; the updater's layout probe then
# reports "reflash needed" instead of silently half-working.
INITRAMFS_SRC="${BOARD_DIR}/initramfs/init"
ABSEL_SRC="${BR2_EXTERNAL_RNSBOX_PATH}/package/rnsbox-update/src/abselect"
GEN_CPIO=$(ls "${BUILD_DIR}"/linux-*/usr/gen_init_cpio 2>/dev/null | head -1)
if [ -x "${GEN_CPIO}" ] && [ -f "${TARGET_DIR}/bin/busybox" ] && \
   [ -f "${INITRAMFS_SRC}" ]; then
	CPIO_LIST="${BINARIES_DIR}/initramfs.list"
	{
		echo "dir /bin 0755 0 0"
		echo "dir /proc 0755 0 0"
		echo "dir /sys 0755 0 0"
		echo "dir /dev 0755 0 0"
		echo "dir /boot 0755 0 0"
		echo "dir /realroot 0755 0 0"
		echo "nod /dev/console 0600 0 0 c 5 1"
		echo "file /init ${INITRAMFS_SRC} 0755 0 0"
		echo "file /bin/busybox ${TARGET_DIR}/bin/busybox 0755 0 0"
		[ -f "${ABSEL_SRC}" ] && echo "file /rnsbox-abselect ${ABSEL_SRC} 0755 0 0"
	} > "${CPIO_LIST}"
	"${GEN_CPIO}" "${CPIO_LIST}" > "${FW}/initramfs.cpio"
	if command -v gzip >/dev/null 2>&1; then
		gzip -9 -c "${FW}/initramfs.cpio" > "${FW}/initramfs.gz" \
			&& rm -f "${FW}/initramfs.cpio"
	else
		# keep the uncompressed cpio; config.txt points at initramfs.gz, so
		# rename rather than ship nothing (the firmware loads either)
		mv -f "${FW}/initramfs.cpio" "${FW}/initramfs.gz"
	fi
	rm -f "${CPIO_LIST}"
	echo "post-image: A/B initramfs built ($(stat -c%s "${FW}/initramfs.gz") bytes)"
else
	echo "WARNING: A/B initramfs NOT built (missing gen_init_cpio/busybox) — the updater needs this initramfs" >&2
fi
# Slot-decision lib also lives on the boot FAT (the initramfs prefers it, so
# an updater transaction can refresh it together with the kernel).
if [ -f "${ABSEL_SRC}" ]; then
	cp -f "${ABSEL_SRC}" "${FW}/rnsbox-abselect"
fi

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
