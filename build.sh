#!/bin/bash
# build.sh — build RNSBox images, all boards, one repository.
#
# The repo is a Buildroot external tree; board files live under board/,
# defconfigs under configs/, shared packages under package/. Two board kinds:
#
#   rpi0-2w (external)  — official Buildroot at a pinned commit, cloned once
#                         and shared; no patch series; builds with
#                         BR2_EXTERNAL=<repo> into its own O= directory.
#   licheerv-nano       — vendor BSP; the git-am series under
#                         patches/licheerv-nano/ is applied onto the pinned
#                         upstream clone.
#   milkv-duo-01-arm    — same vendor BSP (same pin, same host-tools), but
#                         boots Linux on the SG2000's Cortex-A53: the shared
#                         licheerv-nano series is applied first, then the
#                         ARM64 board delta under patches/milkv-duo-01-arm/
#                         (board dir sg2000_duo01evb_arm64_sd, aarch64
#                         buildroot defconfig, 64bit ramdisk overlay).
#
#   ./build.sh [board] [lite|dvd]     (default board: licheerv-nano, lite)
#   ./build.sh all lite               — every board sequentially
#
# Env overrides: UPSTREAM_URL, UPSTREAM_COMMIT, HOST_TOOLS_URL, WORKDIR
# (lichee); UPSTREAM_BASE, ODIR, BR2_DL_DIR (rpi0-2w); RNSBOX_NPROC caps
# BSP-build parallelism (build/Makefile honors it).
#
# RNS version policy: the shipped `rns` (Reticulum) is the LATEST release at
# build time — build.sh asks PyPI, re-pins package/python-rns and verifies
# the download. RNSBOX_RNS_VERSION forces a version; offline builds fall
# back to the pinned default.
set -e

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
BOARDS="licheerv-nano milkv-duo-01-rv milkv-duo-01-arm milkv-duo-cv1800b rpi0-2w"

BOARD=""
VARIANT=""
for a in "$@"; do
    case "$a" in
        lite|dvd) VARIANT="$a" ;;
        *) BOARD="$a" ;;
    esac
done
VARIANT="${VARIANT:-lite}"
BOARD="${BOARD:-licheerv-nano}"

if [ "$BOARD" = all ]; then
    for b in $BOARDS; do
        "$0" "$b" "$VARIANT" || exit 1
    done
    exit 0
fi

case " $BOARDS " in
    *" $BOARD "*) ;;
    *) echo "unknown board '$BOARD' — expected one of: all $BOARDS" >&2; exit 1 ;;
esac
export RNSBOX_BOARD="$BOARD"

# ---- per-board settings ----------------------------------------------------
case "$BOARD" in
    licheerv-nano)
        BUILD_STYLE=patches
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/sipeed/LicheeRV-Nano-Build.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-d4003f15b35d43ad4842f427050ab2bba0114fa5}"
        HOST_TOOLS_URL="${HOST_TOOLS_URL:-https://github.com/sophgo/host-tools}"
        WORKDIR="${WORKDIR:-LicheeRV-Nano-Build}"
        RNSBOX_IMAGES_DIR="install/soc_sg2002_licheervnano_sd/images"
        # series to git-am, in order (relative to patches/)
        RNSBOX_PATCH_SERIES="licheerv-nano"
        RNSBOX_DEFCONFIG_BOARDS="sg2002_licheervnano_sd"
        RNSBOX_BR_DEFCONFIG=cvitek_SG200X_musl_riscv64_defconfig
        RNSBOX_PACKER=build-rnsbox.sh
        ;;
    milkv-duo-01-arm)
        BUILD_STYLE=patches
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/sipeed/LicheeRV-Nano-Build.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-d4003f15b35d43ad4842f427050ab2bba0114fa5}"
        HOST_TOOLS_URL="${HOST_TOOLS_URL:-https://github.com/sophgo/host-tools}"
        WORKDIR="${WORKDIR:-milkv-duo-01-arm}"
        RNSBOX_IMAGES_DIR="install/soc_sg2000_duo01evb_arm64_sd/images install/soc_sg2002_duo_arm64_sd/images"
        # the shared BSP series, then the ARM64 board delta on top
        RNSBOX_PATCH_SERIES="licheerv-nano milkv-duo-01-arm"
        # the arm64 family: ONE arm64 kernel superset (identical defconfigs,
        # per-board DTBs) + ONE aarch64 buildroot rootfs, consumed by every
        # board in the list; partition shapes live in each board dir
        RNSBOX_DEFCONFIG_BOARDS="sg2000_duo01evb_arm64_sd sg2002_duo_arm64_sd"
        RNSBOX_BR_DEFCONFIG=cvitek_SG200X_64bit_defconfig
        # buildroot userspace toolchain (Arm GNU 11.3, glibc) inside host-tools;
        # kernel/u-boot/fsbl keep the vendor-pinned linaro 6.3.1 via SDK env
        RNSBOX_BR_TOOLCHAIN_SUBPATH=gcc/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu
        RNSBOX_PACKER=build-rnsbox-arm.sh
        ;;
    milkv-duo-01-rv)
        # Milk-V Duo Module 01 (Evaluation Board) — SG2000, C906B RISC-V boot.
        # Same vendor BSP + patch-series flow as the licheerv-nano; a
        # sibling board dir (sg2000_duomodule01_sd) carries the SG2000
        # grafts. RNSBOX_BSP_BOARD tells the BSP's build-rnsbox.sh which
        # board dir to defconfig and pack.
        BUILD_STYLE=patches
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/sipeed/LicheeRV-Nano-Build.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-d4003f15b35d43ad4842f427050ab2bba0114fa5}"
        HOST_TOOLS_URL="${HOST_TOOLS_URL:-https://github.com/sophgo/host-tools}"
        WORKDIR="${WORKDIR:-LicheeRV-Nano-Build-duo01}"
        RNSBOX_IMAGES_DIR="install/soc_sg2000_duomodule01_sd/images"
        export RNSBOX_BSP_BOARD=sg2000_duomodule01_sd
        ;;
    milkv-duo-cv1800b)
        # Universal CVITEK RV64 entry: ONE buildroot rootfs + per-board
        # boot artifacts for the whole family - cv1800b (Milk-V Duo 64M,
        # C906-only phobos kernel), sg2000_duomodule01 (Duo Module 01 EVB)
        # and sg2002_licheervnano (LicheeRV Nano). The vendor build dirs
        # are per board (build/<board>, ramdisk/build/<board>,
        # install/soc_<board>) while the buildroot output is shared, so
        # both chip kernels - the CV180X-vs-CV181X Kconfig choice forbids
        # a single Image for phobos and mars - and the one rootfs coexist
        # in this workdir. A new sibling board = a new board dir + boot
        # blobs in the BSP, zero new porting.
        BUILD_STYLE=patches
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/sipeed/LicheeRV-Nano-Build.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-d4003f15b35d43ad4842f427050ab2bba0114fa5}"
        HOST_TOOLS_URL="${HOST_TOOLS_URL:-https://github.com/sophgo/host-tools}"
        WORKDIR="${WORKDIR:-LicheeRV-Nano-Build-cvitek-universal}"
        RNSBOX_UNIVERSAL_BOARDS="${RNSBOX_UNIVERSAL_BOARDS:-cv1800b_sophpi_duo_sd sg2000_duomodule01_sd sg2002_licheervnano_sd}"
        # first-class board of this entry; sg2000/sg2002 images land in
        # install/soc_<board>/images next to it
        RNSBOX_IMAGES_DIR="install/soc_cv1800b_sophpi_duo_sd/images"
        ;;
    rpi0-2w)
        BUILD_STYLE=external
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/buildroot/buildroot.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-679b9ead7620bbf193620d1ebf56f53c1764d37a}"  # 2026.02.3
        DEFCONFIG=rnsbox_rpi0_2w_64_defconfig
        ;;
esac

# ---- rns latest-tracking ----------------------------------------------------
# <tree-root> is the buildroot top (patch style) or the external tree; both
# keep python-rns at package/python-rns. [dl-dir] overrides the cache guess.
rnsbox_track_latest_rns() {
    local pkgdir=""
    for cand in "$1/package/python-rns" "$1/buildroot/package/python-rns"; do
        [ -f "$cand/python-rns.mk" ] && pkgdir="$cand" && break
    done
    if [ -z "$pkgdir" ]; then
        echo ">> python-rns package not present, skipping latest-rns tracking"
        return 0
    fi
    local mk="$pkgdir/python-rns.mk"
    local hashf="$pkgdir/python-rns.hash"
    local dl="${2:-$(cd "$pkgdir/../.." && pwd)/dl}"
    local want="${RNSBOX_RNS_VERSION:-}"
    if [ -z "$want" ]; then
        want="$(curl -fsSL --max-time 30 https://pypi.org/pypi/rns/json 2>/dev/null | jq -r '.info.version')" || want=""
    fi
    if [ -z "$want" ] || [ "$want" = "null" ]; then
        echo ">> PyPI unreachable and RNSBOX_RNS_VERSION unset — keeping the pinned $(sed -n 's/^PYTHON_RNS_VERSION ?*= //p' "$mk")"
        return 0
    fi

    local base sha url
    base="$(curl -fsSL --max-time 30 "https://pypi.org/pypi/rns/$want/json")" || base=""
    sha="$(printf '%s' "$base" | jq -r '.urls[] | select(.packagetype=="sdist") | .digests.sha256' | head -1)"
    url="$(printf '%s' "$base" | jq -r '.urls[] | select(.packagetype=="sdist") | .url' | head -1)"
    if [ -z "$sha" ] || [ "$sha" = "null" ] || [ -z "$url" ] || [ "$url" = "null" ]; then
        echo ">> could not fetch the rns $want sdist digest — keeping the pinned version" >&2
        return 0
    fi

    sed -i "s/^PYTHON_RNS_VERSION ?*= .*/PYTHON_RNS_VERSION ?= $want/" "$mk"
    { echo "# fetched from PyPI by build.sh at $(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
      echo "sha256  $sha  rns-$want.tar.gz"; } > "$hashf"

    if [ ! -s "$dl/python-rns/rns-$want.tar.gz" ]; then
        mkdir -p "$dl/python-rns"
        local tmp="$dl/python-rns/.rns-$want.download"
        if curl -fsSL --retry 3 --max-time 300 -o "$tmp" "$url" \
           && [ "$(sha256sum "$tmp" | cut -d' ' -f1)" = "$sha" ]; then
            mv "$tmp" "$dl/python-rns/rns-$want.tar.gz"
        else
            rm -f "$tmp"
            echo ">> WARNING: could not pre-seed rns $want into the dl cache; the pinned-URL fallback may fail" >&2
        fi
    fi
    echo ">> rns (Reticulum) version: $want (latest from PyPI, sha256-verified)"
}

# ---- rootfs slimming (image-size) -------------------------------------------
# The lite images ship a rootfs sized to content + margin, NOT to the SD
# card: S99resizefs (CVITEK) / S02resizefs (rpi) grow the partition and the
# filesystem to fill the card on first boot, so the factory size only sets
# the download size. rnsbox_slim_bsp wires the shared target-slimmer and the
# size/format knobs into every BSP (patches-style) tree; the external
# rpi0-2w flow gets the same treatment straight in
# configs/rnsbox_rpi0_2w_64_defconfig + board/raspberrypi.
rnsbox_slim_bsp() {
    # 1. the shared post-build slimmer (idempotent, deletes only provably
    #    unused content — see board/rnsbox/slim-target.sh for the keep-list
    #    reasoning: pip/setuptools stay, the portal Update button stays).
    mkdir -p buildroot/board/rnsbox
    cp -f "$HERE/board/rnsbox/slim-target.sh" buildroot/board/rnsbox/slim-target.sh
    chmod 0755 buildroot/board/rnsbox/slim-target.sh
    # 2. every cvitek defconfig in the tree:
    #    - python3 pyc-only (the .py sources are ~half the python payload;
    #      bytecode keeps rnsd/portal startup fast, and pip re-adds .py for
    #      anything it installs or upgrades);
    #    - ext4 root reservation 5% -> 1% (first-boot resize makes it moot);
    #    - hook the slimmer as the buildroot post-build script.
    for dc in buildroot/configs/cvitek_*.defconfig; do
        [ -f "$dc" ] || continue
        sed -i 's/^BR2_PACKAGE_PYTHON3_PY_PYC=y/# BR2_PACKAGE_PYTHON3_PY_PYC is not set\nBR2_PACKAGE_PYTHON3_PYC_ONLY=y/' "$dc"
        sed -i 's/^BR2_TARGET_ROOTFS_EXT2_RESBLKS=.*/BR2_TARGET_ROOTFS_EXT2_RESBLKS=1/' "$dc"
        sed -i 's#^BR2_ROOTFS_POST_BUILD_SCRIPT=.*#BR2_ROOTFS_POST_BUILD_SCRIPT="board/rnsbox/slim-target.sh"#' "$dc"
    done
    # 3. lite rootfs fixed size: build-rnsbox.sh re-stamps this literal at
    #    pack time (its restore() resets the DVD shape), so the pack script
    #    itself carries the slim value. 112M = slimmed content (~80M) +
    #    ext4 metadata/journal + growth slack; S99resizefs grows it on the
    #    card anyway. buildroot fails the fs step only if content outgrows.
    sed -i 's/BR2_TARGET_ROOTFS_EXT2_SIZE="200M"/BR2_TARGET_ROOTFS_EXT2_SIZE="112M"/' build-rnsbox.sh
}

# ---- board build steps ------------------------------------------------------
rnsbox_build_bsp_generic() {
    # Shared by every patches-style board: the BSP workdir is already in place
    # (upstream clone + host-tools + series applied + packages synced).
    echo ">> initial full build (first run compiles Rust from source; be patient)"
    # cap BSP parallelism; point the buildroot userspace at the board's
    # toolchain when the board overrides it (ARM64: Arm GNU 11.3 from host-tools)
    export RNSBOX_NPROC="${RNSBOX_NPROC:-4}"
    if [ -n "$RNSBOX_BR_TOOLCHAIN_SUBPATH" ]; then
        export RNSBOX_BR_TOOLCHAIN_PATH="$PWD/host-tools/$RNSBOX_BR_TOOLCHAIN_SUBPATH"
        # buildroot resolves a custom preinstalled external toolchain via
        # `which $(prefix)-gcc` — put the board's buildroot toolchain on PATH.
        export PATH="$RNSBOX_BR_TOOLCHAIN_PATH/bin:$PATH"
    fi
    # WSL note: Windows PATH interop appends /mnt/c entries (some with
    # spaces) that buildroot's dependencies check rejects — strip them.
    export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    source build/cvisetup.sh
    export PATH=/usr/sbin:/sbin:$PATH
    sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="112M"/' \
        "buildroot/configs/$RNSBOX_BR_DEFCONFIG"
    for b in $RNSBOX_DEFCONFIG_BOARDS; do
        sed -i 's#\(<partition label="ROOTFS" size_in_kb=\)"[0-9]*"#\1"1638400"#' \
            "build/boards/sg200x/$b/partition/partition_sd.xml"
    done
    sed -i '/"usb.disk0",/d; /"usb.disk0.size",/d' \
        build/tools/common/sd_tools/genimage_rootless.cfg
    chmod 0755 buildroot/board/cvitek/SG200X/overlay/etc/init.d/S* 2>/dev/null || true
    export RNSBOX_WITH_DVD=0
    # one kernel superset: every arm64 family board uses a byte-identical
    # kernel defconfig, so pre-seeding board N's O= dir from board 1's
    # finished build makes its build_all a no-op rebuild (only its DTBs,
    # fip and boot.sd get regenerated with its own memmap — the per-board
    # kernel-dts pass re-links cvi_board_memmap.h and rebuilds all dtbs).
    # On a fresh tree the first board builds honestly and seeds the rest.
    first="${RNSBOX_DEFCONFIG_BOARDS%% *}"
    rest="${RNSBOX_DEFCONFIG_BOARDS#* }"
    if [ "$rest" != "$first" ]; then
        for b in $rest; do
            for pair in linux_5.10/build u-boot-2021.10/build; do
                k0="$pair/$first"; k1="$pair/$b"
                if [ -d "$k0" ] && [ ! -d "$k1" ]; then
                    echo ">> pre-seeding $k1 from $k0 (identical arm64 config)"
                    cp -a "$k0" "$k1"
                fi
            done
        done
    fi
    for b in $RNSBOX_DEFCONFIG_BOARDS; do
        defconfig "$b"
        build_all
    done

    if [ "$VARIANT" = dvd ]; then
        echo ">> fetching latest-stable client apps for the DVD"
        ./fetch-clients.sh
    fi
    echo ">> packaging [$VARIANT] image"
    "./$RNSBOX_PACKER" "$VARIANT"
}

rnsbox_build_licheerv_nano() {
    # Initial full compile. build-rnsbox.sh (inside the patched tree) only
    # (re)packs, so one build_all is needed first; lite-shaped so the first
    # pack needs no clients ISO; the pack then produces the requested variant.
    rnsbox_build_bsp_generic
}

rnsbox_build_milkv_duo_01_arm() {
    rnsbox_build_bsp_generic
}

rnsbox_build_milkv_duo_01() {
    # Same shape as the licheerv-nano build; the board dir (and with it the
    # DRAM map, dts, defconfigs) differs, everything else — headless strip,
    # shared packages, gadget/router/portal/rnsd series — is identical.
    # WSL note: Windows PATH interop appends /mnt/c entries (some with
    # spaces) that buildroot rejects - strip them.
    export PATH=$(printf '%s' "$PATH" | tr ':' '\n' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    echo ">> initial full build (first run compiles Rust from source; be patient)"
    source build/cvisetup.sh
    defconfig sg2000_duomodule01_sd
    export PATH=/usr/sbin:/sbin:$PATH
    sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="112M"/' \
        buildroot/configs/cvitek_SG200X_musl_riscv64_defconfig
    sed -i 's#\(<partition label="ROOTFS" size_in_kb=\)"[0-9]*"#\1"1638400"#' \
        build/boards/sg200x/sg2000_duomodule01_sd/partition/partition_sd.xml
    sed -i '/"usb.disk0",/d; /"usb.disk0.size",/d' \
        build/tools/common/sd_tools/genimage_rootless.cfg
    chmod 0755 buildroot/board/cvitek/SG200X/overlay/etc/init.d/S* 2>/dev/null || true
    export RNSBOX_WITH_DVD=0
    defconfig sg2000_duomodule01_sd

    # Crash-resilient staging: each heavy phase is stamped in the BSP tree
    # once it completes. A flaky build host (VM resets kill the build
    # process but not the tree) then resumes instead of redoing the
    # u-boot/kernel middleware passes every cycle; build-rnsbox.sh below
    # re-runs the buildroot pack phase, the only one that can really
    # change between runs. Delete .rnsbox-done-* to force a phase again
    # (e.g. after changing this board's patches).
    local stage
    for stage in build_uboot build_kernel build_ramboot build_osdrv \
                 build_3rd_party build_middleware pack_cfg; do
        if [ ! -e ".rnsbox-done-$stage" ]; then
            "$stage" || return 1
            touch ".rnsbox-done-$stage"
        fi
    done

    if [ "$VARIANT" = dvd ]; then
        echo ">> fetching latest-stable client apps for the DVD"
        ./fetch-clients.sh
    fi
    echo ">> packaging [$VARIANT] image"
    ./build-rnsbox.sh "$VARIANT"
}

# ---- universal CVITEK RV64 flow (milkv-duo-cv1800b) --------------------------
rnsbox_univ_chip_dir() {
    case "$1" in
        cv1800b_*) echo cv180x ;;
        *)         echo sg200x ;;
    esac
}

rnsbox_univ_stage() {
    # $1 board dir name, $2 resume tag. Runs the vendor stage list once per
    # board; .rnsbox-done-<tag>-<stage> stamps in the BSP tree make every
    # relaunch resume where it died (delete one to force a phase again).
    local board="$1" tag="$2" stage
    source build/cvisetup.sh
    defconfig "$board"
    export PATH=/usr/sbin:/sbin:$PATH
    # lite shape, same as every RNSBox lite image: 112M rootfs ext4 in a
    # 1.6G partition (S99resizefs grows it on first boot), NCM-only gadget.
    sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="112M"/' \
        buildroot/configs/cvitek_SG200X_musl_riscv64_defconfig
    sed -i 's#\(<partition label="ROOTFS" size_in_kb=\)"[0-9]*"#\1"1638400"#' \
        "build/boards/$(rnsbox_univ_chip_dir "$board")/$board/partition/partition_sd.xml"
    sed -i '/"usb.disk0",/d; /"usb.disk0.size",/d' \
        build/tools/common/sd_tools/genimage_rootless.cfg
    chmod 0755 buildroot/board/cvitek/SG200X/overlay/etc/init.d/S* 2>/dev/null || true
    export RNSBOX_WITH_DVD=0
    export NPROC="${RNSBOX_NPROC:-2}"
    export MAKEFLAGS="NPROC=${NPROC}${MAKEFLAGS:+ $MAKEFLAGS}"
    defconfig "$board"

    # The ramdisk workspace (target/, configs/) must exist before
    # build_kernel: its pack_boot packs boot.itb from it, and a fresh tree
    # has none. create_ramdisk_folder (what build_ramboot calls) populates
    # it WITHOUT touching the kernel O= dir - it must stay before
    # build_kernel, while build_ramboot itself (make ramboot -> kernel-dts)
    # needs the kernel O= configured, i.e. it must stay AFTER build_kernel.
    if [ ! -e ".rnsbox-done-$tag-ramdisk_ws" ]; then
        create_ramdisk_folder || return 1
        touch ".rnsbox-done-$tag-ramdisk_ws"
    fi

    for stage in build_uboot build_kernel build_ramboot build_osdrv \
                 build_3rd_party build_middleware pack_cfg; do
        [ -e ".rnsbox-done-$tag-$stage" ] && continue
        "$stage" || return 1
        touch ".rnsbox-done-$tag-$stage"
    done
}

rnsbox_univ_stage_sg2002() {
    # sg2002_licheervnano reuses the sg2000 pass (same mars kernel recipe):
    # hardlink the per-board kernel O= + ramdisk dirs, then let make flip
    # the .config to the sg2002 board defconfig, rebuild the DTBs against
    # the sg2002 memmap and repack boot.sd. osdrv/middleware kos are
    # chip-arch (SG200X) outputs and come along with the hardlinks.
    source build/cvisetup.sh
    defconfig sg2002_licheervnano_sd
    export PATH=/usr/sbin:/sbin:$PATH
    export RNSBOX_WITH_DVD=0

    if [ ! -e .rnsbox-done-sg2002-build_uboot ]; then
        build_uboot || return 1
        touch .rnsbox-done-sg2002-build_uboot
    fi
    [ -e .rnsbox-done-sg2002-kernel-reuse ] && return 0

    rm -rf build/sg2002_licheervnano_sd ramdisk/build/sg2002_licheervnano_sd
    # REAL copies, not hardlinks: the vendor make rewrites files like the
    # kernel .config truncate-in-place (cp without unlink), which would
    # push sg2002 content into the sg2000 originals through a shared inode.
    cp -a build/sg2000_duomodule01_sd build/sg2002_licheervnano_sd
    cp -a ramdisk/build/sg2000_duomodule01_sd ramdisk/build/sg2002_licheervnano_sd
    # drop the copied DTBs so kernel-dts rebuilds them against the sg2002
    # memmap (the memmap header is included at dtc preprocessing time)
    rm -f build/sg2002_licheervnano_sd/arch/riscv/boot/dts/cvitek/*.dtb
    build_kernel || return 1
    touch .rnsbox-done-sg2002-kernel-reuse
}

rnsbox_build_milkv_duo_cv1800b() {
    # WSL note: Windows PATH interop appends /mnt/c entries (some with
    # spaces) that buildroot rejects - strip them.
    export PATH=$(printf '%s' "$PATH" | tr ':' '\n' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    export RNSBOX_NPROC="${RNSBOX_NPROC:-2}"

    local b
    for b in $RNSBOX_UNIVERSAL_BOARDS; do
        case "$b" in
            cv1800b_*)
                echo ">> [$b] phobos kernel pass (64MB board, full build)"
                rnsbox_univ_stage "$b" cv1800b || return 1
                ;;
            sg2000_*)
                echo ">> [$b] mars kernel pass (also builds the shared rootfs)"
                rnsbox_univ_stage "$b" sg2000 || return 1
                ;;
            sg2002_*)
                echo ">> [$b] reuse pass (mars kernel objects, own DTB/fip/uboot)"
                rnsbox_univ_stage_sg2002 || return 1
                ;;
        esac
    done

    # image assembly: per-board boot blobs + THE shared rootfs.ext4.
    # build-rnsbox.sh re-defconfigs per board, packs the rootfs once per
    # run (incremental) and burns one image per board.
    for b in $RNSBOX_UNIVERSAL_BOARDS; do
        echo ">> packing [$VARIANT] image for $b"
        RNSBOX_BSP_BOARD="$b" ./build-rnsbox.sh "$VARIANT" || return 1
    done
}


rnsbox_build_rpi0_2w() {
    # Set up by the external flow below: $UPSTREAM (pristine clone at the
    # pin), $EXT (this repo, WSL-local copy when run from /mnt), $ODIR.
    # WSL note: Windows PATH interop appends /mnt/c entries (some with
    # spaces) that buildroot rejects — strip them.
    export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    export PATH=/usr/sbin:/sbin:$PATH

    echo ">> configuring ($DEFCONFIG) + building (first run compiles everything; be patient)"
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT" "$DEFCONFIG"
    rnsbox_pin_dl

    if [ "$VARIANT" = dvd ]; then
        echo ">> fetching latest-stable client apps for the DVD"
        ( cd "$EXT" && scripts/fetch-clients.sh )
    fi
    echo ">> packaging [$VARIANT] image"
    "$EXT/scripts/build-variant.sh" "$VARIANT" "$UPSTREAM" "$ODIR" "$EXT"
}

rnsbox_build() {
    case "$BOARD" in
        licheerv-nano)    rnsbox_build_licheerv_nano ;;
        milkv-duo-01-arm) rnsbox_build_milkv_duo_01_arm ;;
        milkv-duo-01-rv)  rnsbox_build_milkv_duo_01 ;;
        milkv-duo-cv1800b)     rnsbox_build_milkv_duo_cv1800b ;;
        rpi0-2w)          rnsbox_build_rpi0_2w ;;
    esac
}

# ---- external-tree flow (rpi0-2w) -------------------------------------------
if [ "${BUILD_STYLE:-patches}" = external ]; then
    SHORT=$(echo "$UPSTREAM_COMMIT" | cut -c1-8)
    UPSTREAM_BASE="${UPSTREAM_BASE:-$HOME/rnsbox-work/upstream}"
    UPSTREAM="$UPSTREAM_BASE/buildroot-$SHORT"
    mkdir -p "$UPSTREAM_BASE"

    # wget shim: buildroot's dl-wrapper passes no read-timeout and the
    # BR2_WGET default only retries 3 times, so a stalled mirror hangs
    # forever and one flaky mirror can fail a cold build. Flags come AFTER
    # "$@" on purpose — wget is last-wins, so these override the dl-wrapper
    # defaults instead of being overridden by them.
    mkdir -p "$UPSTREAM_BASE/bin"
    printf '#!/bin/bash\nexec /usr/bin/wget "$@" --tries=5 --timeout=60 --read-timeout=30 --continue\n' > "$UPSTREAM_BASE/bin/wget"
    chmod +x "$UPSTREAM_BASE/bin/wget"
    export PATH="$UPSTREAM_BASE/bin:$PATH"

    if [ ! -d "$UPSTREAM/.git" ]; then
        echo ">> cloning upstream base (shared by every external board): $UPSTREAM_URL @ $UPSTREAM_COMMIT"
        git clone "$UPSTREAM_URL" "$UPSTREAM"
        git -C "$UPSTREAM" checkout --detach "$UPSTREAM_COMMIT"
    else
        git -C "$UPSTREAM" checkout -q --detach "$UPSTREAM_COMMIT"
    fi
    [ -z "$(git -C "$UPSTREAM" status --porcelain)" ] || {
        echo ">> $UPSTREAM is dirty — refusing to build from a modified upstream" >&2; exit 1; }

    # The external tree must live on a fast filesystem: when the repo is
    # checked out under /mnt (WSL/drvfs), rsync it into the WSL fs.
    EXT="$HERE"
    case "$HERE" in
        /mnt/*)
            EXT="$UPSTREAM_BASE/rnsbox-ext"
            echo ">> syncing repo -> $EXT (drvfs is too slow for buildroot)"
            rsync -a --delete --exclude .git --exclude dl --exclude out "$HERE/" "$EXT/"
            ;;
    esac

    DL="${BR2_DL_DIR:-$UPSTREAM_BASE/dl}"
    ODIR="${ODIR:-$UPSTREAM_BASE/out/$BOARD}"
    export BR2_DL_DIR="$DL"
    mkdir -p "$DL" "$ODIR"

    # The env var only feeds the kconfig DEFAULT for BR2_DL_DIR — 2026.02
    # keeps "$(TOPDIR)/dl" in .config, silently pulling downloads into the
    # pristine upstream clone. Pin the shared cache into .config explicitly.
    rnsbox_pin_dl() {
        sed -i -e 's|^BR2_DL_DIR=.*|BR2_DL_DIR="'"$DL"'"|' "$ODIR/.config"
        grep -q '^BR2_DL_DIR=' "$ODIR/.config" || echo "BR2_DL_DIR=\"$DL\"" >> "$ODIR/.config"
        make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT" olddefconfig
    }

    rnsbox_track_latest_rns "$EXT" "$DL"
    rnsbox_build

    echo ">> done — image(s) under $ODIR/images/"
    ls -lh "$ODIR/images/rnsbox-$BOARD-$VARIANT.img" 2>/dev/null | tail -2 || true
    exit 0
fi

# ---- patch-series flow (licheerv-nano, milkv-duo-01-arm) ----------------------
# WORKDIR must be absolute: the flow cd's into it, and the shared-package
# sync + build steps below keep referencing "$WORKDIR/..." afterwards.
case "$WORKDIR" in
    /*) ;;
    *) WORKDIR="$PWD/$WORKDIR" ;;
esac

if [ ! -e "$WORKDIR/.git" ]; then
    echo ">> cloning upstream base: $UPSTREAM_URL @ $UPSTREAM_COMMIT"
    git clone "$UPSTREAM_URL" "$WORKDIR"
    cd "$WORKDIR"
    git checkout "$UPSTREAM_COMMIT"
    if [ -n "$HOST_TOOLS_URL" ]; then
        echo ">> fetching cross toolchain: $HOST_TOOLS_URL"
        [ -e host-tools ] || git clone --depth=1 "$HOST_TOOLS_URL" host-tools
    fi
    for series in $RNSBOX_PATCH_SERIES; do
        echo ">> applying RNSBox patch series from $HERE/patches/$series"
        git -c user.name=rnsbox -c user.email=rnsbox@localhost am "$HERE"/patches/"$series"/*.patch
    done
else
    echo ">> reusing existing $WORKDIR"
    cd "$WORKDIR"
    if [ -n "$HOST_TOOLS_URL" ]; then
        [ -e host-tools ] || git clone --depth=1 "$HOST_TOOLS_URL" host-tools
    fi
fi

# Same wget shim as the external flow above: the BSP's inner buildroot also
# downloads through buildroot's dl-wrapper, which has no read-timeout and
# only 3 tries by default — a flaky mirror killed a cold lichee build once.
# (After the clone block: git clone refuses a non-empty destination dir.)
# Flags after "$@" so they override the dl-wrapper defaults (last-wins).
mkdir -p "$WORKDIR/bin"
printf '#!/bin/bash\nexec /usr/bin/wget "$@" --tries=5 --timeout=60 --read-timeout=30 --continue\n' > "$WORKDIR/bin/wget"
chmod +x "$WORKDIR/bin/wget"
export PATH="$WORKDIR/bin:$PATH"

# Shared packages live in ONE place — <repo>/package/ — no matter how many
# boards this repo grows to. The patch series deliberately does not vendor
# them (its last patch deletes the copies the early series added); copy the
# current shared versions into the BSP buildroot before configuring.
echo ">> syncing shared packages from $HERE/package"
for p in rnsbox-portal python-rns python-lxmf python-nomadnet; do
    rm -rf "$WORKDIR/buildroot/package/$p"
    cp -a "$HERE/package/$p" "$WORKDIR/buildroot/package/$p"
done
# The un-vendoring BSP patch removed the matching `source` lines from
# buildroot/package/Config.in together with the vendored copies; without
# them kconfig silently drops BR2_PACKAGE_PYTHON_RNS / PYTHON_LXMF /
# PYTHON_NOMADNET / RNSBOX_PORTAL and the rootfs ships without rnsd and
# the portal. Restore the lines so the synced dirs are visible.
BR_PKG_CONFIG="$WORKDIR/buildroot/package/Config.in"
for p in rnsbox-portal python-rns python-lxmf python-nomadnet; do
    grep -q "source \"package/$p/Config.in\"" "$BR_PKG_CONFIG" || \
        printf 'source "package/%s/Config.in"\n' "$p" >> "$BR_PKG_CONFIG"
done

# rootfs slimming: sync the slimmer + flip the size/format knobs (see the
# rnsbox_slim_bsp comment block above for what and why)
echo ">> slimming knobs: pyc-only python, 1% ext4 reservation, 112M lite rootfs, slim-target hook"
rnsbox_slim_bsp

rnsbox_track_latest_rns "$PWD"
rnsbox_build

echo ">> done — image(s) under $WORKDIR/$RNSBOX_IMAGES_DIR/"
for d in $RNSBOX_IMAGES_DIR; do
    ls -lh "$WORKDIR/$d"/*-"$VARIANT".img 2>/dev/null | tail -2 || true
done
