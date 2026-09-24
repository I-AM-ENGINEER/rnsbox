#!/bin/bash
# build.sh — build RNSBox from scratch, from nothing but a patch series.
#
# Clones the pinned upstream base, applies the RNSBox patch series for the
# requested board (the *.patch files under patches/<board>/) and builds.
#
#   ./build.sh [board] [lite|dvd]     (default board: licheerv-nano, lite)
#
# Boards:
#   licheerv-nano   Sipeed LicheeRV Nano-e / Nano-e W  (SG2002, riscv64-musl)
#   rpi0-2w         Raspberry Pi Zero 2 W              (aarch64-musl)
#
# A lone `lite`/`dvd` arg keeps working (board defaults to licheerv-nano).
#
# Env overrides (per board, see boards/<board>.sh): UPSTREAM_URL,
# UPSTREAM_COMMIT, HOST_TOOLS_URL, WORKDIR.
#
# RNS version policy: the shipped `rns` (Reticulum) is the LATEST release at
# build time. build.sh asks PyPI, rewrites package/python-rns (version + sha256
# hash) before compiling, and verifies the download. Set RNSBOX_RNS_VERSION to
# force a specific version; offline builds fall back to the pinned default in
# the patch series with a warning.
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

if [ ! -f "$HERE/boards/$BOARD.sh" ]; then
    echo "unknown board '$BOARD' — expected one of: $(cd "$HERE/boards" && ls *.sh | sed 's/\.sh$//' | tr '\n' ' ')" >&2
    exit 1
fi
. "$HERE/boards/$BOARD.sh"

# rnsbox_track_latest_rns <builddir> — point package/python-rns at the newest
# rns release on PyPI (version + sha256), verifying against the PyPI digests.
rnsbox_track_latest_rns() {
    local pkgdir="$1/package/python-rns"
    local mk="$pkgdir/python-rns.mk"
    local hashf="$pkgdir/python-rns.hash"
    [ -f "$mk" ] || { echo ">> python-rns package not present, skipping latest-rns tracking"; return 0; }

    local want="${RNSBOX_RNS_VERSION:-}"
    if [ -z "$want" ]; then
        want="$(curl -fsSL --max-time 30 https://pypi.org/pypi/rns/json 2>/dev/null | jq -r '.info.version')" || want=""
    fi
    if [ -z "$want" ] || [ "$want" = "null" ]; then
        echo ">> PyPI unreachable and RNSBOX_RNS_VERSION unset — keeping the pinned $(sed -n 's/^PYTHON_RNS_VERSION = //p' "$mk") from the patch series"
        return 0
    fi

    local sha
    sha="$(curl -fsSL --max-time 30 "https://pypi.org/pypi/rns/$want/json" | jq -r '.urls[] | select(.packagetype=="sdist") | .digests.sha256' | head -1)"
    if [ -z "$sha" ] || [ "$sha" = "null" ]; then
        echo ">> could not fetch the rns $want sdist digest — keeping the pinned version" >&2
        return 0
    fi

    sed -i "s/^PYTHON_RNS_VERSION = .*/PYTHON_RNS_VERSION = $want/" "$mk"
    { echo "# fetched from PyPI by build.sh at $(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
      echo "sha256  $sha  rns-$want.tar.gz"; } > "$hashf"
    echo ">> rns (Reticulum) version: $want (latest from PyPI, sha256-verified)"
}

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
