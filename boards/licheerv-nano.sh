# boards/licheerv-nano.sh — Sipeed LicheeRV Nano-e / Nano-e W (SG2002, riscv64)
#
# Upstream base is the Sipeed board support package; the RNSBox delta lives in
# patches/licheerv-nano/.

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/sipeed/LicheeRV-Nano-Build.git}"
UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-d4003f15b35d43ad4842f427050ab2bba0114fa5}"
HOST_TOOLS_URL="${HOST_TOOLS_URL:-https://github.com/sophgo/host-tools}"
WORKDIR="${WORKDIR:-LicheeRV-Nano-Build}"
RNSBOX_IMAGES_DIR="install/soc_sg2002_licheervnano_sd/images"

rnsbox_build() {
    # Initial full compile. build-rnsbox.sh only (re)packs, so we need one
    # build_all first to compile u-boot/kernel/osdrv/buildroot(+Rust). Shape it
    # lite so this first pack needs no clients ISO; build-rnsbox.sh then
    # produces the requested variant (and, for dvd, builds + auto-sizes to the
    # ISO).
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

    if [ "$VARIANT" = dvd ]; then
        echo ">> fetching latest-stable client apps for the DVD"
        ./fetch-clients.sh
    fi
    echo ">> packaging [$VARIANT] image"
    ./build-rnsbox.sh "$VARIANT"
}
