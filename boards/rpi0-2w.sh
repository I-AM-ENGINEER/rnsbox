# boards/rpi0-2w.sh — Raspberry Pi Zero 2 W (BCM2710A1, quad Cortex-A53, aarch64)
#
# Upstream base is official Buildroot (LTS) — the Raspberry Pi Foundation
# kernel and firmware come through Buildroot's own packages. The RNSBox delta
# lives in patches/rpi0-2w/.

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/buildroot/buildroot.git}"
UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-679b9ead7620bbf193620d1ebf56f53c1764d37a}"  # 2026.02.3
WORKDIR="${WORKDIR:-buildroot-rpi0-2w}"
RNSBOX_IMAGES_DIR="output/images"

rnsbox_build() {
    # The series ships configs/rnsbox_rpi0_2w_64_defconfig. A plain `make`
    # after that builds kernel, rootfs and the genimage sdcard.
    echo ">> configuring and building (first run compiles the toolchain; be patient)"
    # WSL note: Windows PATH interop appends /mnt/c entries (some with
    # spaces) that buildroot's dependency check rejects — strip them.
    export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    export PATH=/usr/sbin:/sbin:$PATH
    make rnsbox_rpi0_2w_64_defconfig
    make

    if [ "$VARIANT" = dvd ]; then
        echo ">> fetching latest-stable client apps for the DVD"
        ./fetch-clients.sh
    fi
    echo ">> packaging [$VARIANT] image"
    ./build-rnsbox.sh "$VARIANT"
}
