#!/bin/bash
# build.sh — RNSBox images, one external Buildroot flow for every board.
#
# The repository is the BR2_EXTERNAL tree: board files under board/,
# defconfigs under configs/, shared packages under package/. One pinned
# upstream Buildroot clone is shared by every board; each board adds its
# defconfig and its post-image script. CVITEK first-stage blobs (fip.bin)
# are inputs to the board's post-image, not a second build system.
#
#   ./build.sh [board]     (default: rpi-universal)
#   ./build.sh all         — every board sequentially
#
# Boards: rpi-universal pc-x86_64 orangepi-r1 milkv-duo-cv1800b-rv64
#         sg2000-duo-s-arm64 sg2000-duomodule01-rv64
#         sg2002-licheerv-nano-rv64 sg2002-duo256m-rv64
#         sg2000-duo-s-rv64
#
# Env overrides: UPSTREAM_URL, UPSTREAM_COMMIT, UPSTREAM_BASE, ODIR,
# BR2_DL_DIR. Every image is lite (one NCM gadget; no clients DVD).
#
# RNS version policy: the shipped `rns` (Reticulum) is the LATEST release at
# build time — build.sh asks PyPI, re-pins package/python-rns and verifies
# the download. RNSBOX_RNS_VERSION forces a version; offline builds fall
# back to the pinned default.
set -e

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

# ---- image version stamp (read by rnsbox-sysupdate for update decisions) ----
# Same formula as the CI release job's tag: r<UTC date>-<short sha>. On CI,
# GITHUB_SHA is set by Actions and the release tag is created from the same
# value, so the stamped image matches its release tag (same-UTC-day builds,
# the normal case). Local builds get dev-<date>; the updater treats an
# unknown/unstamped running version as manual-apply-only.
if [ -n "${GITHUB_SHA:-}" ]; then
	export RNSBOX_BUILD_VER="r$(date -u +%Y.%m.%d)-${GITHUB_SHA:0:7}"
else
	export RNSBOX_BUILD_VER="dev-$(date -u +%Y.%m.%d)"
fi
export RNSBOX_BUILD_VER

BOARD=""
VARIANT=""
for a in "$@"; do
    case "$a" in
        lite|dvd) VARIANT="$a" ;;
        *) BOARD="$a" ;;
    esac
done
VARIANT="${VARIANT:-lite}"
BOARD="${BOARD:-rpi-universal}"

BOARDS="rpi-universal pc-x86_64 orangepi-r1 milkv-duo-cv1800b-rv64 sg2000-duo-s-arm64 sg2000-duomodule01-rv64 sg2002-licheerv-nano-rv64 sg2002-duo256m-rv64 sg2000-duo-s-rv64"

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

# Every remaining board is the same external flow: a defconfig + a board
# post-image script. No vendor BSP, no packer, no per-board shell pipeline.
case "$BOARD" in
    rpi-universal)             DEFCONFIG=rnsbox_rpi_universal_defconfig ;;
    pc-x86_64)                 DEFCONFIG=rnsbox_pc_x86_64_defconfig ;;
    orangepi-r1)               DEFCONFIG=rnsbox_orangepi_r1_defconfig ;;
    milkv-duo-cv1800b-rv64)    DEFCONFIG=rnsbox_milkv_duo_cv1800b_mainline_defconfig ;;
    sg2000-duo-s-arm64)        DEFCONFIG=rnsbox_duo_s_arm64_mainline_defconfig ;;
    sg2000-duomodule01-rv64)   DEFCONFIG=rnsbox_sg2000_duomodule01_mainline_defconfig ;;
    sg2002-licheerv-nano-rv64) DEFCONFIG=rnsbox_licheerv_nano_mainline_defconfig ;;
    sg2002-duo256m-rv64)       DEFCONFIG=rnsbox_duo256m_mainline_defconfig ;;
    sg2000-duo-s-rv64)         DEFCONFIG=rnsbox_duo_s_mainline_defconfig ;;
esac
BUILD_STYLE=external
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/buildroot/buildroot.git}"
UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-679b9ead7620bbf193620d1ebf56f53c1764d37a}"  # 2026.02.3
export RNSBOX_BOARD="$BOARD"

# ---- rns latest-tracking ----------------------------------------------------
# <tree-root> is the external tree; it keeps python-rns at
# package/python-rns. [dl-dir] overrides the cache guess.
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

rnsbox_build_external() {
    export PATH=$(printf '%s' "$PATH" | tr ':' '
' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    export PATH=/usr/sbin:/sbin:$PATH
    if [ "$VARIANT" != lite ]; then
        echo ">> $BOARD: lite only, building lite"
        VARIANT=lite
    fi
    export RNSBOX_VARIANT="$VARIANT"
    echo ">> configuring ($DEFCONFIG) + building (first run compiles everything; be patient)"
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT" "$DEFCONFIG"
    rnsbox_pin_dl
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT"
}

# ---- external flow (every board) ---------------------------------------------------
if [ "${BUILD_STYLE:-external}" = external ]; then
    # configs/rnsbox_*_defconfig are generated from configs/base.config +
    # configs/boards/*.config. A stale copy silently builds the wrong
    # userspace, so fail before the expensive clone/build.
    "$HERE/scripts/gen-defconfigs.sh" --check

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
            # DrvFs reports every file as 0644, so rsync -a cannot carry the
            # executable bit recorded in the git index. Re-apply it for the
            # scripts that must stay runnable (init.d, helpers) — otherwise
            # the image ships a non-executable rcS and boots without services.
            ( cd "$HERE" && git ls-files -s ) | while read -r mode _ _ path; do
                [ "$mode" = 100755 ] || continue
                if [ -f "$EXT/$path" ]; then chmod 0755 "$EXT/$path"; fi
            done
            ;;
    esac

    DL="${BR2_DL_DIR:-$UPSTREAM_BASE/dl}"
    ODIR="${ODIR:-$UPSTREAM_BASE/out/$BOARD}"
    export BR2_DL_DIR="$DL"
    mkdir -p "$DL" "$ODIR"

    # Shared packages are SITE_METHOD=local: buildroot rsyncs them into
    # output/build/<pkg>-<ver> ONCE at extract time and never again, so an
    # incremental external build happily recompiles a STALE copy (this is
    # how a "fresh" RPi/EVB image shipped a week-old portal). Drop the
    # extracted copies and their stamps; the next make re-extracts them
    # from $EXT. aic8800 is the
    # out-of-tree WiFi driver source shipped in board/sg2000-mainline/.
    # hostapd: board/common/buildroot-patches/hostapd patches the nl80211
    # driver (monitor-less AP mgmt frames), which is only applied at extract.
    for p in rnsbox-update rnsbox-portal python-rns python-lxmf python-nomadnet python-ble-reticulum rnsbox-python-bluezero aic8800 hostapd wpa_supplicant; do
        rm -rf "$ODIR"/build/"$p"-* 2>/dev/null || :
    done

    # The env var only feeds the kconfig DEFAULT for BR2_DL_DIR — 2026.02
    # keeps "$(TOPDIR)/dl" in .config, silently pulling downloads into the
    # pristine upstream clone. Pin the shared cache into .config explicitly.
    rnsbox_pin_dl() {
        sed -i -e 's|^BR2_DL_DIR=.*|BR2_DL_DIR="'"$DL"'"|' "$ODIR/.config"
        grep -q '^BR2_DL_DIR=' "$ODIR/.config" || echo "BR2_DL_DIR=\"$DL\"" >> "$ODIR/.config"
        # Out-of-tree package patches (hostapd) live in board/common. Buildroot
        # only reads this list from .config, and the defconfig we just imported
        # does not know about the external tree's absolute path.
        local pdir="$EXT/board/common/buildroot-patches"
        if [ -d "$pdir" ]; then
            sed -i -e 's|^BR2_GLOBAL_PATCH_DIR=.*|BR2_GLOBAL_PATCH_DIR="'"$pdir"'"|' "$ODIR/.config"
            grep -q '^BR2_GLOBAL_PATCH_DIR=' "$ODIR/.config" || echo "BR2_GLOBAL_PATCH_DIR=\"$pdir\"" >> "$ODIR/.config"
        fi
        # Python picks its stdlib options at configure time and Buildroot does
        # not rebuild a package when the kconfig changes. A tree configured
        # before PYEXPAT was enabled ships without pyexpat/xml, and the BLE
        # stack (dbus/bluezero import xml.etree) then refuses to start. Drop
        # the stale tree so the option takes effect.
        if grep -q '^BR2_PACKAGE_PYTHON3_PYEXPAT=y' "$ODIR/.config"; then
            _py_xml=
            for _x in "$ODIR"/target/usr/lib/python3.*/xml; do
                [ -d "$_x" ] && { _py_xml=$_x; break; }
            done
            if [ -z "$_py_xml" ] && ls -d "$ODIR"/build/python3-3.14* >/dev/null 2>&1; then
                echo ">> python3 was built without PYEXPAT (no xml/ in the target) - rebuilding it"
                rm -rf "$ODIR"/build/python3-3.14*
            fi
        fi
        make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT" olddefconfig
    }

    rnsbox_track_latest_rns "$EXT" "$DL"

    # Buildroot writes BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE into build/linux-*/
    # only when it configures the tree, so a hand-edited .config survives every
    # later build. That is how the Module 01 EVB once shipped a kernel with the
    # C906 errata/vector/extensions switched off: u-boot printed "Starting
    # kernel ..." and the board went silent. When the tree disagrees with the
    # board config on a user-selectable option, throw the tree away so the
    # board config wins again.
    rnsbox_drop_stale_kernel_config() {
        local kdir cfg opt want have stale kcfg
        kdir=$(ls -d "$ODIR"/build/linux-* 2>/dev/null | head -n1)
        [ -n "$kdir" ] && [ -f "$kdir/.config" ] || return 0
        kcfg=$(sed -n 's/^BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE="\(.*\)"/\1/p' "$ODIR/.config" | head -n1)
        [ -n "$kcfg" ] && [ -f "$kcfg" ] || return 0
        stale=""
        for opt in CONFIG_RISCV_ISA_V CONFIG_ERRATA_THEAD CONFIG_RISCV_DMA_NONCOHERENT \
                   CONFIG_RISCV_ISA_ZBC CONFIG_RISCV_NONSTANDARD_CACHE_OPS CONFIG_DEVMEM; do
            want=$(grep -E "^${opt}(=| is not set)" "$kcfg" | head -n1)
            [ -n "$want" ] || continue
            have=$(grep -E "^${opt}=" "$kdir/.config" | head -n1)
            [ -n "$have" ] || have="# ${opt} is not set"
            [ "$want" = "$have" ] || stale="$stale $opt"
        done
        if [ -n "$stale" ]; then
            echo ">> kernel .config disagrees with $(basename "$kcfg") on$stale" >&2
            echo ">> discarding $kdir so the board config is applied again" >&2
            rm -rf "$kdir"
        fi
    }
    rnsbox_drop_stale_kernel_config

    rnsbox_build_external

    echo ">> done — image(s) under $ODIR/images/"
    ls -lh "$ODIR/images/rnsbox-$BOARD.img" "$ODIR/images/rnsbox-$BOARD-$VARIANT.img" 2>/dev/null | tail -2 || true
    exit 0
fi

