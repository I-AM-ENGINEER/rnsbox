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
BOARDS="licheerv-nano milkv-duo-01-arm rpi0-2w"

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
        RNSBOX_DEFCONFIG_BOARD=sg2002_licheervnano_sd
        RNSBOX_BR_DEFCONFIG=cvitek_SG200X_musl_riscv64_defconfig
        RNSBOX_PACKER=build-rnsbox.sh
        ;;
    milkv-duo-01-arm)
        BUILD_STYLE=patches
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/sipeed/LicheeRV-Nano-Build.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-d4003f15b35d43ad4842f427050ab2bba0114fa5}"
        HOST_TOOLS_URL="${HOST_TOOLS_URL:-https://github.com/sophgo/host-tools}"
        WORKDIR="${WORKDIR:-milkv-duo-01-arm}"
        RNSBOX_IMAGES_DIR="install/soc_sg2000_duo01evb_arm64_sd/images"
        # the shared BSP series, then the ARM64 board delta on top
        RNSBOX_PATCH_SERIES="licheerv-nano milkv-duo-01-arm"
        RNSBOX_DEFCONFIG_BOARD=sg2000_duo01evb_arm64_sd
        RNSBOX_BR_DEFCONFIG=cvitek_SG200X_64bit_defconfig
        # buildroot userspace toolchain (Arm GNU 11.3, glibc) inside host-tools;
        # kernel/u-boot/fsbl keep the vendor-pinned linaro 6.3.1 via SDK env
        RNSBOX_BR_TOOLCHAIN_SUBPATH=gcc/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu
        RNSBOX_PACKER=build-rnsbox-arm.sh
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
    defconfig "$RNSBOX_DEFCONFIG_BOARD"
    export PATH=/usr/sbin:/sbin:$PATH
    sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="200M"/' \
        "buildroot/configs/$RNSBOX_BR_DEFCONFIG"
    sed -i 's#\(<partition label="ROOTFS" size_in_kb=\)"[0-9]*"#\1"1638400"#' \
        "build/boards/sg200x/$RNSBOX_DEFCONFIG_BOARD/partition/partition_sd.xml"
    sed -i '/"usb.disk0",/d; /"usb.disk0.size",/d' \
        build/tools/common/sd_tools/genimage_rootless.cfg
    chmod 0755 buildroot/board/cvitek/SG200X/overlay/etc/init.d/S* 2>/dev/null || true
    export RNSBOX_WITH_DVD=0
    defconfig "$RNSBOX_DEFCONFIG_BOARD"
    build_all

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
        rpi0-2w)          rnsbox_build_rpi0_2w ;;
    esac
}

# ---- external-tree flow (rpi0-2w) -------------------------------------------
if [ "${BUILD_STYLE:-patches}" = external ]; then
    SHORT=$(echo "$UPSTREAM_COMMIT" | cut -c1-8)
    UPSTREAM_BASE="${UPSTREAM_BASE:-$HOME/rnsbox-work/upstream}"
    UPSTREAM="$UPSTREAM_BASE/buildroot-$SHORT"
    mkdir -p "$UPSTREAM_BASE"

    # wget shim: buildroot's dl-wrapper passes no read-timeout, so a stalled
    # mirror hangs wget forever instead of failing over to the next mirror.
    mkdir -p "$UPSTREAM_BASE/bin"
    printf '#!/bin/bash\nexec /usr/bin/wget --read-timeout=30 --continue "$@"\n' > "$UPSTREAM_BASE/bin/wget"
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

# Shared packages live in ONE place — <repo>/package/ — no matter how many
# boards this repo grows to. The patch series deliberately does not vendor
# them (its last patch deletes the copies the early series added); copy the
# current shared versions into the BSP buildroot before configuring.
echo ">> syncing shared packages from $HERE/package"
for p in rnsbox-portal python-rns python-lxmf python-nomadnet; do
    rm -rf "$WORKDIR/buildroot/package/$p"
    cp -a "$HERE/package/$p" "$WORKDIR/buildroot/package/$p"
done

rnsbox_track_latest_rns "$PWD"
rnsbox_build

echo ">> done — image(s) under $WORKDIR/$RNSBOX_IMAGES_DIR/"
ls -lh "$WORKDIR/$RNSBOX_IMAGES_DIR"/*-"$VARIANT".img 2>/dev/null | tail -2 || true
