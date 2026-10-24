#!/bin/bash
# HT-HD01-V2 image stamping.  The production build produces a squashfs rootfs
# next to the kernel uImage; an initramfs variant is used for TFTP-to-RAM
# testing.  Both are copied under stable names.
set -eu

BOARD_DIR="$(dirname "$0")"
BINARIES_DIR="${BINARIES_DIR:-${BASE_DIR}/images}"
BOARD="${RNSBOX_BOARD:-ht-hd01-v2}"

PREFIX="rnsbox-${BOARD}"

# The kernel writes a streaming LZMA header; U-Boot needs the real size.
# Use the system interpreter: Buildroot's host-python may omit _lzma.
PYTHON=python3
command -v "$PYTHON" >/dev/null 2>&1 || PYTHON="${HOST_DIR:-}/bin/python3"

if [ -f "${BINARIES_DIR}/uImage" ]; then
	"$PYTHON" "${BOARD_DIR}/fix-lzma-uimage.py" "${BINARIES_DIR}/uImage"
	cp -f "${BINARIES_DIR}/uImage" "${BINARIES_DIR}/${PREFIX}-uImage"
fi
if [ -f "${BINARIES_DIR}/rootfs.squashfs" ]; then
	cp -f "${BINARIES_DIR}/rootfs.squashfs" \
		"${BINARIES_DIR}/${PREFIX}-rootfs.squashfs"
fi
( cd "${BINARIES_DIR}" && sha256sum ${PREFIX}-* > "${PREFIX}-SHA256SUMS" 2>/dev/null || true )

echo "=================== IMAGE OK [${PREFIX}] $(date) ==================="
