#!/bin/bash
# build.sh — build RNSBox from scratch, from nothing but this patch set.
#
# Clones the pinned upstream base, fetches the cross toolchain, applies the
# RNSBox patch series (the *.patch files next to this script), and builds.
#
#   ./build.sh [lite|dvd]        (default: lite)
#
# Env overrides: UPSTREAM_URL, UPSTREAM_COMMIT, HOST_TOOLS_URL, WORKDIR
#
# Notes:
#   * The first build compiles a Rust host toolchain from source (needed for
#     python-cryptography on riscv64-musl). Expect a long first run.
#   * lite  = NCM-only image (no extra downloads).
#     dvd   = lite + a read-only disc of the LATEST-stable Reticulum client
#             apps, fetched fresh from GitHub by ./fetch-clients.sh. The dvd
#             image is auto-sized to the clients ISO (currently ~3.3 GB; use a
#             microSD comfortably larger, e.g. 8 GB).
set -e

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/sipeed/LicheeRV-Nano-Build.git}"
UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-d4003f15b35d43ad4842f427050ab2bba0114fa5}"
HOST_TOOLS_URL="${HOST_TOOLS_URL:-https://github.com/sophgo/host-tools}"
WORKDIR="${WORKDIR:-LicheeRV-Nano-Build}"
VARIANT="${1:-lite}"
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

case "$VARIANT" in lite|dvd) ;; *) echo "usage: $0 [lite|dvd]"; exit 1 ;; esac

# 1. upstream base + toolchain + patches (only on a fresh checkout)
if [ ! -e "$WORKDIR/.git" ]; then
	echo ">> cloning upstream base: $UPSTREAM_URL @ $UPSTREAM_COMMIT"
	git clone "$UPSTREAM_URL" "$WORKDIR"
	cd "$WORKDIR"
	git checkout "$UPSTREAM_COMMIT"
	echo ">> fetching cross toolchain: $HOST_TOOLS_URL"
	[ -e host-tools ] || git clone --depth=1 "$HOST_TOOLS_URL" host-tools
	echo ">> applying RNSBox patch series from $HERE/patches"
	git -c user.name=rnsbox -c user.email=rnsbox@localhost am "$HERE"/patches/*.patch
else
	echo ">> reusing existing $WORKDIR"
	cd "$WORKDIR"
	[ -e host-tools ] || git clone --depth=1 "$HOST_TOOLS_URL" host-tools
fi

# 2. initial full compile. build-rnsbox.sh only (re)packs, so we need one
#    build_all first to compile u-boot/kernel/osdrv/buildroot(+Rust). Shape it
#    lite so this first pack needs no clients ISO; build-rnsbox.sh then produces
#    the requested variant (and, for dvd, builds + auto-sizes to the ISO).
echo ">> initial full build (first run compiles Rust from source; be patient)"
source build/cvisetup.sh
defconfig sg2002_licheervnano_sd
export PATH=/usr/sbin:/sbin:$PATH
sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="200M"/' \
	buildroot/configs/cvitek_SG200X_musl_riscv64_defconfig
sed -i 's#\(<partition label="ROOTFS" size_in_kb=\)"[0-9]*"#\1"1638400"#' \
	build/boards/sg200x/sg2002_licheervnano_sd/partition/partition_sd.xml
sed -i '/"usb.disk0",/d; /"usb.disk0.size",/d' \
	build/tools/common/sd_tools/genimage_rootless.cfg
chmod 0755 buildroot/board/cvitek/SG200X/overlay/etc/init.d/S* 2>/dev/null || true
export RNSBOX_WITH_DVD=0
defconfig sg2002_licheervnano_sd
build_all

# 3. produce the requested variant image
if [ "$VARIANT" = dvd ]; then
	echo ">> fetching latest-stable client apps for the DVD"
	./fetch-clients.sh
fi
echo ">> packaging [$VARIANT] image"
./build-rnsbox.sh "$VARIANT"

echo ">> done — image(s) under install/soc_sg2002_licheervnano_sd/images/"
ls -lh install/soc_sg2002_licheervnano_sd/images/*-"$VARIANT".img 2>/dev/null | tail -2
