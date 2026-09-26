# boards/rpi0-2w.sh — Raspberry Pi Zero 2 W (BCM2710A1, quad Cortex-A53, aarch64)
#
# External-tree board: NO patch series. The rnsbox repo itself is the
# BR2_EXTERNAL tree (external.desc at the root); official Buildroot (LTS) is
# cloned once per pin and shared by every external-tree board, each building
# into its own O= directory. The Raspberry Pi Foundation kernel and firmware
# come through Buildroot's own packages.

BUILD_STYLE=external
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/buildroot/buildroot.git}"
UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-679b9ead7620bbf193620d1ebf56f53c1764d37a}"  # 2026.02.3
DEFCONFIG=rnsbox_rpi0_2w_64_defconfig

rnsbox_build() {
    # Set up by build.sh for external boards:
    #   $UPSTREAM — pristine buildroot clone at UPSTREAM_COMMIT
    #   $EXT      — this repo (WSL-local copy when running from /mnt)
    #   $ODIR     — per-board build output (O=)
    # WSL note: Windows PATH interop appends /mnt/c entries (some with
    # spaces) that buildroot's dependency check rejects — strip them.
    export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    export PATH=/usr/sbin:/sbin:$PATH

    echo ">> configuring ($DEFCONFIG) + building (first run compiles everything; be patient)"
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT" "$DEFCONFIG"
    rnsbox_pin_dl

    if [ "$VARIANT" = dvd ]; then
        echo ">> fetching latest-stable client apps for the DVD"
        ( cd "$EXT" && ./fetch-clients.sh )
    fi
    echo ">> packaging [$VARIANT] image"
    "$EXT/scripts/build-variant.sh" "$VARIANT" "$UPSTREAM" "$ODIR" "$EXT"
}
