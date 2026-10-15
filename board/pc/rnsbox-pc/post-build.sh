#!/bin/sh
# RNSBox PC/VM post-build: place our grub.cfg (the grub2 package installed
# its default one), stage grub's 1st-stage boot.img next to the image for
# genimage, and keep every overlay init script executable (the copy tooling
# does not preserve the +x bit; both the busybox rc and the portal's
# subprocess execve() reject non-executable S* scripts).
set -e

BOARD_DIR="$(dirname "$0")"

mkdir -p "${TARGET_DIR}/boot/grub"
cp -f "${BOARD_DIR}/grub.cfg" "${TARGET_DIR}/boot/grub/grub.cfg"

# grub 1st stage, required by genimage (same trick as buildroot board/pc)
cp -f "${TARGET_DIR}/lib/grub/i386-pc/boot.img" "${BINARIES_DIR}"

if [ -d "${TARGET_DIR}/etc/init.d" ]; then
	chmod 0755 "${TARGET_DIR}"/etc/init.d/S* 2>/dev/null || true
fi
