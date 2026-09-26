#!/bin/bash
# RNSBox — package the lite or dvd variant image for an external-tree board.
#
#   scripts/build-variant.sh <lite|dvd> <upstream-dir> <output-dir> <ext-dir>
#
#   lite : NCM-only gadget (no clients ISO), 400M rootfs — the default.
#   dvd  : NCM + read-only clients "DVD" (ISO9660 in the rootfs, auto-sized
#          to the fetched apps) + mass-storage LUN attached by S10usbdev.
#
# The variant touches two spots, then re-runs make (incremental: rootfs
# repack + post-image + genimage only):
#   1. ext-tree overlay ISO  <ext>/board/<b>/rootfs-overlay/opt/rnsbox-clients/usbdisk.iso
#   2. output .config        BR2_TARGET_ROOTFS_EXT2_SIZE (400M lite | ISO + 400M dvd)
# plus the usb.disk0 boot flag (rpi-firmware/ on the boot FAT). The tree is
# restored to the lite state on exit.
set -o pipefail

VARIANT="${1:?variant lite-or-dvd required}"
UP="${2:?upstream dir required}"; O="${3:?output dir required}"; EXT="${4:?external-tree dir required}"
case "$VARIANT" in dvd|lite) ;; *) echo "usage: $0 {lite|dvd} <upstream> <O> <ext>"; exit 1 ;; esac
UP="$(cd "$UP" && pwd)"; O="$(cd "$O" && pwd)"; EXT="$(cd "$EXT" && pwd)"

BOARD_DIR="$EXT/board/raspberrypi/rnsbox-0-2w"
ISO_OVERLAY="$BOARD_DIR/rootfs-overlay/opt/rnsbox-clients/usbdisk.iso"
ISO_STASH=/tmp/rnsbox-usbdisk.iso.stash
FW="$O/images/rpi-firmware"
LITE_EXT2=400M

restore() {
	sed -i -E "s/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE=\"$LITE_EXT2\"/" "$O/.config"
	if [ -f "$ISO_STASH" ] && [ ! -f "$ISO_OVERLAY" ]; then
		mkdir -p "$(dirname "$ISO_OVERLAY")"
		mv "$ISO_STASH" "$ISO_OVERLAY"
	fi
}
trap restore EXIT

MAKE="make -C $UP O=$O BR2_EXTERNAL=$EXT"

if [ "$VARIANT" = dvd ]; then
	echo "--- DVD: (re)build the clients ISO ---"
	( cd "$EXT" && scripts/clients-stick/build-clients-img.sh ) || { echo "FATAL: ISO build"; exit 92; }
	# auto-size rootfs to the (dynamic, latest) clients ISO + ~400M OS.
	ISO_MB=$(( ($(stat -c%s "$ISO_OVERLAY") + 1048575) / 1048576 ))
	EXT2_MB=$(( ISO_MB + 400 ))
	echo "--- DVD: ISO ${ISO_MB}M -> rootfs ${EXT2_MB}M ---"
	sed -i -E "s/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE=\"${EXT2_MB}M\"/" "$O/.config"
	: > "$FW/usb.disk0"
	echo iso > "$FW/usb.disk0.size"
else
	echo "--- LITE: NCM-only, strip ISO + shrink rootfs ---"
	[ -f "$ISO_OVERLAY" ] && mv "$ISO_OVERLAY" "$ISO_STASH"
	rm -f "$O/target/opt/rnsbox-clients/usbdisk.iso" "$FW/usb.disk0" "$FW/usb.disk0.size"
	sed -i -E "s/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE=\"$LITE_EXT2\"/" "$O/.config"
fi

echo "--- re-make (repack rootfs + image) ---"
$MAKE olddefconfig || { echo "FATAL: olddefconfig"; exit 93; }
$MAKE || { echo "FATAL: make"; exit 94; }

NEW="$O/images/sdcard.img"
TAGGED="$O/images/rnsbox-${RNSBOX_BOARD:?RNSBOX_BOARD not set}-$VARIANT.img"
cp -f "$NEW" "$TAGGED"
echo "=================== BUILD OK [$VARIANT] $(date) ==================="
ls -lh "$TAGGED" | awk '{print "  "$5"  "$NF}'
echo "DONE-SENTINEL-OK"
