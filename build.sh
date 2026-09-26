#!/bin/bash
# build.sh — build RNSBox images from this repository.
#
# The repo is a BR2_EXTERNAL tree: shared packages under package/, board
# definitions under board/, defconfigs under configs/. Two board styles:
#
#   BUILD_STYLE=external  (rpi0-2w) — official Buildroot is cloned once per
#                         pin and shared by every external board; no patch
#                         series; each board builds into its own O= dir with
#                         BR2_EXTERNAL=<this repo>.
#   BUILD_STYLE=patches   (licheerv-nano) — vendor BSP; the patch series
#                         under patches/<board>/ is git-am'd onto the pinned
#                         upstream clone.
#
#   ./build.sh [board] [lite|dvd]     (default board: licheerv-nano, lite)
#
# Boards:
#   licheerv-nano   Sipeed LicheeRV Nano-e / Nano-e W  (SG2002, riscv64-musl)
#   rpi0-2w         Raspberry Pi Zero 2 W              (aarch64-musl)
#   all             every board, sequentially
#
# A lone `lite`/`dvd` arg keeps working (board defaults to licheerv-nano).
#
# Env overrides (per board, see boards/<board>.sh): UPSTREAM_URL,
# UPSTREAM_COMMIT, HOST_TOOLS_URL, WORKDIR (patch style); UPSTREAM_BASE,
# ODIR, BR2_DL_DIR (external style).
#
# RNS version policy: the shipped `rns` (Reticulum) is the LATEST release at
# build time. build.sh asks PyPI, rewrites package/python-rns (version + sha256
# hash) before compiling, and verifies the download. Set RNSBOX_RNS_VERSION to
# force a specific version; offline builds fall back to the pinned default
# with a warning.
set -e

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

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
    for b in $(cd "$HERE/boards" && ls *.sh | sed 's/\.sh$//'); do
        "$0" "$b" "$VARIANT" || exit 1
    done
    exit 0
fi

if [ ! -f "$HERE/boards/$BOARD.sh" ]; then
    echo "unknown board '$BOARD' — expected one of: all $(cd "$HERE/boards" && ls *.sh | sed 's/\.sh$//' | tr '\n' ' ')" >&2
    exit 1
fi
. "$HERE/boards/$BOARD.sh"
export RNSBOX_BOARD="$BOARD"

# rnsbox_track_latest_rns <tree-root> [dl-dir] — point package/python-rns at
# the newest rns release on PyPI (version + sha256 hash), verify and pre-seed
# the download cache so the build never depends on the version-pinned sdist
# URL. <tree-root> is the buildroot top (patch style) or the external tree
# (external style); both keep python-rns at package/python-rns.
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
        echo ">> PyPI unreachable and RNSBOX_RNS_VERSION unset — keeping the pinned $(sed -n 's/^PYTHON_RNS_VERSION = //p' "$mk")"
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

    sed -i "s/^PYTHON_RNS_VERSION = .*/PYTHON_RNS_VERSION = $want/" "$mk"
    { echo "# fetched from PyPI by build.sh at $(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
      echo "sha256  $sha  rns-$want.tar.gz"; } > "$hashf"

    # Pre-seed the dl cache with the verified sdist so buildroot's downloader
    # never has to resolve the (version-pinned) URL in the .mk.
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

if [ "${BUILD_STYLE:-patches}" = external ]; then
    # ---- external-tree flow: pristine shared buildroot + BR2_EXTERNAL ----
    SHORT=$(echo "$UPSTREAM_COMMIT" | cut -c1-8)
    UPSTREAM_BASE="${UPSTREAM_BASE:-$HOME/rnsbox-work/upstream}"
    UPSTREAM="$UPSTREAM_BASE/buildroot-$SHORT"
    mkdir -p "$UPSTREAM_BASE"

    # wget shim: buildroot's dl-wrapper passes no read-timeout, so a stalled
    # mirror (WSL2 networking hits these) hangs wget forever instead of
    # failing over to the next mirror. --continue is safe for its temp-file
    # scheme. First PATH entry for everything this script builds.
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
    # checked out under /mnt (WSL/drvfs), rsync it into the WSL fs and build
    # from that copy.
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
    # pristine upstream clone. Boards call this right after `make <defconfig>`
    # to pin the shared cache into .config explicitly.
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

# ---- patch-series flow (vendor BSP boards) ----
# 1. upstream base + patches (only on a fresh checkout)
if [ ! -e "$WORKDIR/.git" ]; then
    echo ">> cloning upstream base: $UPSTREAM_URL @ $UPSTREAM_COMMIT"
    git clone "$UPSTREAM_URL" "$WORKDIR"
    cd "$WORKDIR"
    git checkout "$UPSTREAM_COMMIT"
    if [ -n "$HOST_TOOLS_URL" ]; then
        echo ">> fetching cross toolchain: $HOST_TOOLS_URL"
        [ -e host-tools ] || git clone --depth=1 "$HOST_TOOLS_URL" host-tools
    fi
    echo ">> applying RNSBox patch series from $HERE/patches/$BOARD"
    git -c user.name=rnsbox -c user.email=rnsbox@localhost am "$HERE"/patches/"$BOARD"/*.patch
else
    echo ">> reusing existing $WORKDIR"
    cd "$WORKDIR"
    if [ -n "$HOST_TOOLS_URL" ]; then
        [ -e host-tools ] || git clone --depth=1 "$HOST_TOOLS_URL" host-tools
    fi
fi

# 2. latest rns from PyPI, then board-specific full build + variant packaging
rnsbox_track_latest_rns "$PWD"
rnsbox_build

echo ">> done — image(s) under $WORKDIR/$RNSBOX_IMAGES_DIR/"
ls -lh "$WORKDIR/$RNSBOX_IMAGES_DIR"/*-"$VARIANT".img 2>/dev/null | tail -2 || true
