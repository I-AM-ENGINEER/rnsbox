#!/bin/bash
# build.sh — build RNSBox images, all boards, one repository.
#
# The repo is a Buildroot external tree; board files live under board/,
# defconfigs under configs/, shared packages under package/. Two board kinds:
#
#   rpi-universal (external)  — official Buildroot at a pinned commit, cloned once
#                         and shared; no patch series; builds with
#                         BR2_EXTERNAL=<repo> into its own O= directory.
#   pc-x86_64 (external)— same external flow, a different defconfig: a plain
#                         x86_64 BIOS disk (grub in the MBR + one ext4 root)
#                         for QEMU/VirtualBox/VMware — the full router stack
#                         with no hardware attached.
#   milkv-duo-cv1800b-mainline (external) — same external shape, but the
#                         kernel is MAINLINE Linux (pinned kernel.org stable
#                         tag + the board/milkv-duo/linux-patches DTS/driver
#                         deltas) and the boot blobs are the vendor fip.bin
#                         repacked with a mainline FIT
#                         (board/milkv-duo/README.fip.md).
#   sg2002-rv64         — vendor BSP (LicheeRV Nano-e); the git-am series
#                         under patches/licheerv-nano/ is applied onto the
#                         pinned upstream clone.
#   sg2000-rv64         — same BSP, the Duo Module 01 / EVB board dir
#                         (sg2000_duomodule01_sd) in the C906 RISC-V boot.
#   sg2000-arm64 /      — same vendor BSP (same pin, same host-tools), booting
#   sg2002-arm64 /        Linux on the Cortex-A53: the shared licheerv-nano
#   sg2002-licheerv-arm64 series is applied first, then the ARM64 board delta
#                         under patches/milkv-duo-01-arm/ (board dirs
#                         sg2000_duo01evb_arm64_sd, sg2002_duo_arm64_sd and
#                         sg2002_licheervnano_arm64_sd; aarch64 buildroot
#                         defconfig, 64bit ramdisk overlay); one board per
#                         build via RNSBOX_PACKER_BOARDS.
#
#   ./build.sh [board] [lite|dvd]     (default board: sg2002-rv64, lite)
#   ./build.sh all lite               — every board sequentially
#
# Env overrides: UPSTREAM_URL, UPSTREAM_COMMIT, HOST_TOOLS_URL, WORKDIR
# (lichee); UPSTREAM_BASE, ODIR, BR2_DL_DIR (rpi-universal); RNSBOX_NPROC caps
# BSP-build parallelism (build/Makefile honors it).
#
# RNS version policy: the shipped `rns` (Reticulum) is the LATEST release at
# build time — build.sh asks PyPI, re-pins package/python-rns and verifies
# the download. RNSBOX_RNS_VERSION forces a version; offline builds fall
# back to the pinned default.
set -e

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# Board keys = SoC-arch pairs for the CVITEK family (the SG2000/SG2002
# boot core is picked by the board's rv/arm switch, so the IMAGE must say
# which core it boots — evb, duomodule01, licheervnano and duo are the
# same SoCs, not different boards): sg2000-rv64 = Duo Module 01 (EVB),
# sg2000-arm64 = same on the A53, sg2002-rv64 = LicheeRV Nano-e,
# sg2002-arm64 = same on the A53. cv1800b (Duo 64M) is RV-only.
BOARDS="sg2000-rv64 sg2000-arm64 sg2002-rv64 sg2002-arm64 sg2002-licheerv-arm64 milkv-duo-cv1800b rpi-universal orangepi-r1 pc-x86_64 milkv-duo-cv1800b-mainline sg2000-duo-s-arm64-mainline sg2000-duomodule01-mainline sg2002-licheerv-nano-mainline sg2002-duo256m-mainline sg2000-duo-s-mainline"

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
BOARD="${BOARD:-sg2002-rv64}"

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
    sg2002-rv64)
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
    sg2000-arm64|sg2002-arm64|sg2002-licheerv-arm64)
        # One arm64 board per build: the family shares an identical kernel
        # defconfig and aarch64 rootfs, so a single-board run packs its own
        # boot.sd/fip only (the other board's image is the sibling job's).
        # NB: if/fi, not an AND-list — under set -e a false test here would
        # kill the script (same lesson as the packer copy below).
        if [ "$BOARD" = sg2000-arm64 ]; then
            local_board=sg2000_duo01evb_arm64_sd
        elif [ "$BOARD" = sg2002-licheerv-arm64 ]; then
            local_board=sg2002_licheervnano_arm64_sd
        else
            local_board=sg2002_duo_arm64_sd
        fi
        BUILD_STYLE=patches
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/sipeed/LicheeRV-Nano-Build.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-d4003f15b35d43ad4842f427050ab2bba0114fa5}"
        HOST_TOOLS_URL="${HOST_TOOLS_URL:-https://github.com/sophgo/host-tools}"
        WORKDIR="${WORKDIR:-milkv-duo-01-arm}"
        RNSBOX_IMAGES_DIR="install/soc_${local_board}/images"
        # the shared BSP series, then the ARM64 board delta on top
        RNSBOX_PATCH_SERIES="licheerv-nano milkv-duo-01-arm"
        RNSBOX_DEFCONFIG_BOARDS="$local_board"
        RNSBOX_BR_DEFCONFIG=cvitek_SG200X_64bit_defconfig
        # buildroot userspace toolchain (Arm GNU 11.3, glibc) inside host-tools;
        # kernel/u-boot/fsbl keep the vendor-pinned linaro 6.3.1 via SDK env
        RNSBOX_BR_TOOLCHAIN_SUBPATH=gcc/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu
        RNSBOX_PACKER=build-rnsbox-arm.sh
        RNSBOX_PACKER_BOARDS="$local_board"
        ;;
    sg2000-rv64)
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
        # the BSP series - until now every duo-01-rv build reused a
        # pre-patched workspace, so a COLD build (CI, a wiped workdir)
        # applied no patches and had no sg2000_duomodule01_sd board dir
        RNSBOX_PATCH_SERIES="milkv-duo-01"
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
    orangepi-r1)
        # Allwinner H2+ (Cortex-A7) on mainline Linux 6.6 + U-Boot 2024.01.
        # Same external-tree flow as rpi-universal; the image is assembled by
        # board/orangepi-r1/post-image.sh (GPT + sunxi SPL at 8 KiB).
        BUILD_STYLE=external
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/buildroot/buildroot.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-679b9ead7620bbf193620d1ebf56f53c1764d37a}"  # 2026.02.3
        DEFCONFIG=rnsbox_orangepi_r1_defconfig
        ;;
    rpi-universal)
        BUILD_STYLE=external
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/buildroot/buildroot.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-679b9ead7620bbf193620d1ebf56f53c1764d37a}"  # 2026.02.3
        DEFCONFIG=rnsbox_rpi_universal_defconfig
        ;;
    pc-x86_64)
        # PC / virtual machine target: same upstream buildroot as the rpi-universal
        # (one shared clone), its own defconfig. lite shape only — there is
        # no USB gadget in a VM to expose the clients DVD on.
        BUILD_STYLE=external
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/buildroot/buildroot.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-679b9ead7620bbf193620d1ebf56f53c1764d37a}"  # 2026.02.3
        DEFCONFIG=rnsbox_pc_x86_64_defconfig
        ;;
    milkv-duo-cv1800b-mainline)
        # Milk-V Duo 64M (CV1800B, C906) on MAINLINE Linux. External build
        # (shared buildroot clone + dl cache with rpi0-2w); the kernel comes
        # from the pinned kernel.org stable tag with board/milkv-duo patches;
        # the first-stage boot chain stays vendor (fip.bin, see
        # board/milkv-duo/README.fip.md) and only the boot.sd FIT content is
        # ours. RNSBOX_FIP_SRC overrides the boot-blob source.
        BUILD_STYLE=external
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/buildroot/buildroot.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-679b9ead7620bbf193620d1ebf56f53c1764d37a}"  # 2026.02.3
        DEFCONFIG=rnsbox_milkv_duo_cv1800b_mainline_defconfig
        ;;
    sg2000-duo-s-arm64-mainline)
        # Milk-V Duo S on the Cortex-A53 with MAINLINE Linux: upstream
        # sg2000-milkv-duo-s.dts (Ethernet, eMMC, SDIO WiFi, USB), the
        # vendor ARM64 fip.bin (SG2000) and our boot.sd FIT around the
        # mainline arm64 Image/DTB.
        BUILD_STYLE=external
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/buildroot/buildroot.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-679b9ead7620bbf193620d1ebf56f53c1764d37a}"  # 2026.02.3
        DEFCONFIG=rnsbox_duo_s_arm64_mainline_defconfig
        ;;

    sg2000-duomodule01-mainline|sg2002-licheerv-nano-mainline|sg2002-duo256m-mainline|sg2000-duo-s-mainline)
        # The other mainline CVITEK boards: same external buildroot shape as
        # the duo-s-arm64 entry, vendor fip.bin + our boot.sd FIT around the
        # mainline Image/DTB. lite only (no clients DVD on these).
        BUILD_STYLE=external
        UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/buildroot/buildroot.git}"
        UPSTREAM_COMMIT="${UPSTREAM_COMMIT:-679b9ead7620bbf193620d1ebf56f53c1764d37a}"  # 2026.02.3
        case "$BOARD" in
            sg2000-duomodule01-mainline) DEFCONFIG=rnsbox_sg2000_duomodule01_mainline_defconfig ;;
            sg2002-licheerv-nano-mainline) DEFCONFIG=rnsbox_licheerv_nano_mainline_defconfig ;;
            sg2002-duo256m-mainline) DEFCONFIG=rnsbox_duo256m_mainline_defconfig ;;
            sg2000-duo-s-mainline) DEFCONFIG=rnsbox_duo_s_mainline_defconfig ;;
        esac
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
# rpi-universal flow gets the same treatment straight in
# configs/rnsbox_rpi_universal_defconfig + board/raspberrypi.
#
# The exact knob set the rootfs policy depends on (the admin users table,
# locked root, openssh-only). Every cvitek defconfig must carry each knob
# EXACTLY; rnsbox_slim_bsp enforces it and rnsbox_assert_bsp_policy fails
# the build loudly otherwise. build5 shipped images without the admin user
# because a non-application here used to be silent.
RNSBOX_POLICY_KNOBS=(
    'BR2_PACKAGE_PYTHON3_PY_ONLY=y'
    # the idna codec needs the unicodedata C extension; without it EVERY
    # socket.getaddrinfo() on the board raises LookupError: unknown
    # encoding: idna and rnsd dies creating its TCPServerInterface
    # (os._exit, empty /var/log/rnsd.log, wanwatch reboot loop). The
    # vendor 64bit defconfigs shipped without it; see the BSP patch
    # 0018-buildroot-enable-python-unicodedata-on-the-64bit-configs.
    'BR2_PACKAGE_PYTHON3_UNICODEDATA=y'
    '# BR2_PACKAGE_PYTHON3_PY_PYC is not set'
    'BR2_TARGET_ROOTFS_EXT2_RESBLKS=1'
    'BR2_ROOTFS_POST_BUILD_SCRIPT="board/common/slim-target.sh"'
    '# BR2_TARGET_ENABLE_ROOT_LOGIN is not set'
    'BR2_TARGET_GENERIC_ROOT_PASSWD="*"'
    'BR2_ROOTFS_USERS_TABLES="board/common/users.txt"'
    '# BR2_PACKAGE_DROPBEAR is not set'
    'BR2_PACKAGE_OPENSSH_SERVER=y'
)

# Stamp the same policy into a LIVE buildroot .config (the cvitek BSP keeps
# buildroot's .config IN THE SOURCE TREE — there is no output/.config to
# sed, and an incremental build never re-imports the defconfig). Idempotent.
rnsbox_stamp_live_config() {
    local live="$1"
    [ -f "$live" ] || return 0
    sed -i \
        -e 's/^BR2_PACKAGE_PYTHON3_PY_PYC=y/# BR2_PACKAGE_PYTHON3_PY_PYC is not set/' \
        -e 's/^BR2_PACKAGE_PYTHON3_PYC_ONLY=y/# BR2_PACKAGE_PYTHON3_PYC_ONLY is not set/' \
        -e 's/^BR2_TARGET_ROOTFS_EXT2_RESBLKS=.*/BR2_TARGET_ROOTFS_EXT2_RESBLKS=1/' \
        -e 's#^BR2_ROOTFS_POST_BUILD_SCRIPT=.*#BR2_ROOTFS_POST_BUILD_SCRIPT="board/common/slim-target.sh"#' \
        -e 's/^BR2_TARGET_ENABLE_ROOT_LOGIN=y/# BR2_TARGET_ENABLE_ROOT_LOGIN is not set/' \
        -e 's/^BR2_TARGET_GENERIC_ROOT_PASSWD=.*/BR2_TARGET_GENERIC_ROOT_PASSWD="*"/' \
        -e 's#^BR2_ROOTFS_USERS_TABLES=.*#BR2_ROOTFS_USERS_TABLES="board/common/users.txt"#' \
        -e 's/^BR2_PACKAGE_DROPBEAR=y/# BR2_PACKAGE_DROPBEAR is not set/' \
        -e 's/^# BR2_PACKAGE_OPENSSH_SERVER is not set/BR2_PACKAGE_OPENSSH_SERVER=y/' \
        -e 's/^# BR2_PACKAGE_PYTHON3_PY_ONLY is not set$/# py-only selected at build time (rnsbox_slim_bsp)/' \
        -e 's/^# BR2_PACKAGE_PYTHON3_UNICODEDATA is not set$/BR2_PACKAGE_PYTHON3_UNICODEDATA=y/' \
        -e '/^BR2_PACKAGE_SCREEN=y$/d' \
        "$live"
    # PY_ONLY has no line of its own to sed when the file never mentioned
    # the py knobs — append the selection at EOF (kconfig takes the last
    # assignment).
    grep -q '^BR2_PACKAGE_PYTHON3_PY_ONLY=y' "$live" || \
        echo 'BR2_PACKAGE_PYTHON3_PY_ONLY=y' >> "$live"
}

# Verify every policy knob landed in every cvitek defconfig; self-heal a
# knob that is MISSING entirely (append at EOF — kconfig is last-wins), but
# FAIL the build on any value that is present-but-wrong after the seds.
rnsbox_assert_bsp_policy() {
    local bsp="$1" dc k bad
    local matched=0
    # NB: the upstream files are named cvitek_SG200X_64bit_defconfig —
    # UNDERSCORE before defconfig. The historical glob here said
    # cvitek_*.defconfig (dot), never matched a single file, and the
    # `[ -f ] || continue` guard silently ate the miss: every build
    # shipped without the users table / ssh policy (the build5 bug).
    for dc in "$bsp"/buildroot/configs/cvitek_*_defconfig; do
        [ -f "$dc" ] || continue
        matched=1
        bad=0
        for k in "${RNSBOX_POLICY_KNOBS[@]}"; do
            # -xF: the knobs contain regex metacharacters ('*', '"') — a
            # plain -x would never match the wanted line and fail forever.
            if ! grep -qxF "$k" "$dc"; then
                sym="${k%%=*}"
                if [ "${k:0:1}" = "#" ] || ! grep -q "^$sym=" "$dc"; then
                    # an entirely absent symbol (or a not-set comment) is
                    # self-healed by appending at EOF — kconfig takes the
                    # LAST assignment, so the appended value wins.
                    echo "$k" >> "$dc"
                    grep -qxF "$k" "$dc" || { echo "FATAL: $dc: cannot realize policy knob '$k'" >&2; bad=1; }
                    echo ">> $(basename "$dc"): appended missing policy knob: $k"
                else
                    # the symbol IS assigned in this defconfig but the sed
                    # did not make it the wanted value (duplicate assigns,
                    # trailing whitespace, upstream churn) — fail loudly,
                    # this is exactly the silent policy hole build5 shipped.
                    echo "FATAL: $dc: policy knob did not land: wanted '$k', have '$(grep "^$sym=" "$dc" | tail -1)'" >&2
                    bad=1
                fi
            fi
        done
        [ "$bad" = 0 ] || exit 1
    done
    [ "$matched" = 1 ] || {
        echo "FATAL: rnsbox_slim_bsp: no cvitek_*_defconfig found under $bsp/buildroot/configs" >&2
        exit 1
    }
    echo ">> policy knobs verified in every cvitek defconfig (py-only, slim hook, users table, locked root, openssh-only)"
}

rnsbox_slim_bsp() {
    # Anchor everything to the BSP workdir. The caller has cd'd into it, but
    # a step that silently depends on cwd is exactly what once ate the sids
    # (a relative glob matching nothing skips the whole policy unnoticed).
    local bsp="$1"
    if [ -z "$bsp" ] || [ ! -d "$bsp/buildroot" ]; then
        echo "FATAL: rnsbox_slim_bsp: '$bsp' is not a BSP tree (no buildroot/)" >&2
        exit 1
    fi
    # 1. the shared post-build slimmer (idempotent, deletes only provably
    #    unused content — see board/common/slim-target.sh for the keep-list
    #    reasoning: pip/setuptools stay, the portal Update button stays).
    mkdir -p "$bsp/buildroot/board/common"
    cp -f "$HERE/board/common/slim-target.sh" "$bsp/buildroot/board/common/slim-target.sh"
    chmod 0755 "$bsp/buildroot/board/common/slim-target.sh"
    #    - the users table: the unprivileged `admin` operator account,
    #      password LOCKED at build time - the portal sets it when SSH is
    #      enabled (wired into every cvitek defconfig below).
    cp -f "$HERE/board/common/users.txt" "$bsp/buildroot/board/common/users.txt"
    #    - the rest of the shared board/common scripts: the stable hostname,
    #      the unprivileged admin account, the PTY console broker and the zsh
    #      rc. slim-target.sh installs them from its own directory, so a cold
    #      patch-flow build must carry them next to it or it silently skips
    #      every [ -f ] guard and ships without a console/account.
    for f in S10hostname S10consoleuser S11console; do
        if [ -f "$HERE/board/common/$f" ]; then
            cp -f "$HERE/board/common/$f" "$bsp/buildroot/board/common/$f"
            chmod 0755 "$bsp/buildroot/board/common/$f"
        fi
    done
    if [ -f "$HERE/board/common/zshrc" ]; then
        cp -f "$HERE/board/common/zshrc" "$bsp/buildroot/board/common/zshrc"
        chmod 0644 "$bsp/buildroot/board/common/zshrc"
    fi
    #    - uhttpd: TCP_NODELAY on accepted sockets, and tzset() before the
    #      strptime("%Z") that parses If-Modified-Since (musl leaves tzname
    #      NULL, so a browser's cached-CSS request killed the server). The
    #      upstream/external flow gets the same patches from external.mk.
    if [ -d "$bsp/buildroot/package/uhttpd" ]; then
        cp -f "$HERE/board/common/buildroot-patches/uhttpd/"*.patch \
            "$bsp/buildroot/package/uhttpd/"
        chmod 0644 "$bsp/buildroot/package/uhttpd/"*.patch
        # An incremental BSP tree still has .stamp_patched, so copying the
        # patch above would be a no-op. Drop an unpatched build dir; the next
        # make re-extracts and applies the package's patches (uhttpd is small).
        for d in "$bsp"/buildroot/output/build/uhttpd-* \
                 "$bsp"/buildroot/build/uhttpd-*; do
            [ -f "$d/client.c" ] || continue
            if ! grep -q TCP_NODELAY "$d/client.c" || ! grep -q 'tzset();' "$d/file.c"; then
                rm -rf "$d"
            fi
        done
    fi
    # 2. every cvitek defconfig in the tree:
    #    - python3 py-only: the .pyc payload is as big as the .py sources it
    #      duplicates (measured: 61 MB of pyc next to ~55 MB of sources) —
    #      ship sources only and let S51pycompile rebuild the bytecode for
    #      whatever is actually imported, in the background, on first boot;
    #    - ext4 root reservation 5% -> 1% (first-boot resize makes it moot);
    #    - hook the slimmer as the buildroot post-build script.
    local dc
    # NB: cvitek_*_defconfig — underscore suffix (see rnsbox_assert_bsp_policy).
    for dc in "$bsp"/buildroot/configs/cvitek_*_defconfig; do
        [ -f "$dc" ] || continue
        sed -i 's/^BR2_PACKAGE_PYTHON3_PY_PYC=y/# BR2_PACKAGE_PYTHON3_PY_PYC is not set\nBR2_PACKAGE_PYTHON3_PY_ONLY=y/' "$dc"
        sed -i 's/^BR2_PACKAGE_PYTHON3_PYC_ONLY=y/# BR2_PACKAGE_PYTHON3_PYC_ONLY is not set\nBR2_PACKAGE_PYTHON3_PY_ONLY=y/' "$dc"
        sed -i 's/^BR2_TARGET_ROOTFS_EXT2_RESBLKS=.*/BR2_TARGET_ROOTFS_EXT2_RESBLKS=1/' "$dc"
        # the console no longer uses GNU screen (xterm.js streams the PTY
        # broker over the portal's port 80); drop the package if an older
        # series left the knob behind
        sed -i '/^BR2_PACKAGE_SCREEN=y$/d' "$dc"
        sed -i 's#^BR2_ROOTFS_POST_BUILD_SCRIPT=.*#BR2_ROOTFS_POST_BUILD_SCRIPT="board/common/slim-target.sh"#' "$dc"
        # - SSH hardening, mirroring the rpi-universal defconfig: root
        #   login closed + root password locked ('*'), the admin users
        #   table, and openssh as THE ssh daemon (the overlay S50sshd from
        #   the ssh-gate patch keeps it off until the portal enables it;
        #   dropbear must never come back as a second daemon - the two
        #   sshd lines are already the upstream state, kept as seds so an
        #   upstream flip cannot quietly reintroduce it).
        sed -i 's/^BR2_TARGET_ENABLE_ROOT_LOGIN=y/# BR2_TARGET_ENABLE_ROOT_LOGIN is not set/' "$dc"
        sed -i 's/^BR2_TARGET_GENERIC_ROOT_PASSWD=.*/BR2_TARGET_GENERIC_ROOT_PASSWD="*"/' "$dc"
        sed -i 's#^BR2_ROOTFS_USERS_TABLES=.*#BR2_ROOTFS_USERS_TABLES="board/common/users.txt"#' "$dc"
        sed -i 's/^BR2_PACKAGE_DROPBEAR=y/# BR2_PACKAGE_DROPBEAR is not set/' "$dc"
        sed -i 's/^# BR2_PACKAGE_OPENSSH_SERVER is not set/BR2_PACKAGE_OPENSSH_SERVER=y/' "$dc"
        #   - kill the stale "# BR2_PACKAGE_PYTHON3_PY_ONLY is not set"
        #     the PY_PYC flip leaves BELOW its inserted PY_ONLY=y line:
        #     kconfig takes the LAST assignment of a symbol, so without
        #     this the py-only knob silently cancels itself (a plain
        #     comment leaves the symbol at its default n - identical to
        #     an explicit "not set").
        sed -i 's/^# BR2_PACKAGE_PYTHON3_PY_ONLY is not set$/# py-only selected at build time (rnsbox_slim_bsp)/' "$dc"
    done
    rnsbox_assert_bsp_policy "$bsp"
    # live config(s): the cvitek BSP keeps buildroot's .config in the source
    # tree (no O= dir), and a reused workspace builds incrementally without
    # re-importing the defconfig — stamp the policy into it so a warm build
    # picks the knobs up even if the re-import were skipped.
    rnsbox_stamp_live_config "$bsp/buildroot/.config"
    rnsbox_stamp_live_config "$bsp/buildroot/output/.config"
    # first-boot bytecode warm-up (see the script): backgrounded, never
    # blocks rcS, so the py-only rootfs keeps its fast first rnsd start
    if [ -d "$bsp/buildroot/board/cvitek/SG200X/overlay/etc/init.d" ]; then
        cp -f "$HERE/board/common/S51pycompile" "$bsp/buildroot/board/cvitek/SG200X/overlay/etc/init.d/S51pycompile"
        chmod 0755 "$bsp/buildroot/board/cvitek/SG200X/overlay/etc/init.d/S51pycompile"
    fi
    # 3. lite rootfs fixed size: the pack scripts re-stamp this literal at
    #    pack time (their restore() resets the DVD shape), so the pack
    #    scripts themselves carry the slim value. 512M factory ext4 = py-only
    #    content + years of headroom (the empty ext4 space xz-crushes to
#    nothing, so download traffic does not grow; S99resizefs
    #    grows it on the card anyway. buildroot fails the fs step only if
    #    content outgrows. The arm64 family packs through build-rnsbox-arm.sh
    #    (same shape, own copy of the literal).
    local packer
    for packer in "$bsp/build-rnsbox.sh" "$bsp/build-rnsbox-arm.sh"; do
        # NB: `if`, not `[ -f ] &&` — under set -e a false test in an AND-list
        # kills the script (this ate the duo flows on CI: only the arm64 flow
        # carries build-rnsbox-arm.sh).
        if [ -f "$packer" ]; then
            # stamp whatever the packer currently carries (a reused workdir's
            # packer may say 200M, 128M or anything a previous era left) —
            # BOTH the assignment lines AND the packer's own internal
            # re-stamp commands: its lite path rewrites the defconfig to its
            # literal at pack time and re-imports it, overriding everything
            # stamped before it ran.
            sed -i -e 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
                   -e 's/BR2_TARGET_ROOTFS_EXT2_SIZE="128M"/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
                   -e 's/BR2_TARGET_ROOTFS_EXT2_SIZE="200M"/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
                   -e 's/BR2_TARGET_ROOTFS_EXT2_SIZE="160M"/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' "$packer"
        fi
    done
}

# ---- board build steps ------------------------------------------------------
rnsbox_build_bsp_generic() {
    # Shared by every patches-style board: the BSP workdir is already in place
    # (upstream clone + host-tools + series applied + packages synced).
    echo ">> initial full build (first run compiles Rust from source; be patient)"
    # BSP/buildroot parallelism: default to every core the box has. The
    # old hard -j4 left 12 cores idle on a 16-core build host and was a
    # major share of the pack-phase wall clock. Overridable for small-RAM
    # hosts (RNSBOX_NPROC=4 ./build.sh ...).
    export RNSBOX_NPROC="${RNSBOX_NPROC:-$(nproc)}"
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
    sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
        "buildroot/configs/$RNSBOX_BR_DEFCONFIG"
    # incremental builds never re-import the defconfig: stamp the live
    # .config too, or the fs step keeps whatever size a previous era set.
    # The cvitek BSP keeps buildroot's .config IN THE SOURCE TREE; the
    # output/.config path below is a legacy no-op kept for external flows.
    if [ -f buildroot/.config ]; then
        sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
            buildroot/.config
    fi
    if [ -f buildroot/.config ]; then
        sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
            buildroot/.config
    fi
    if [ -f buildroot/output/.config ]; then
        sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
            buildroot/output/.config
    fi
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
    "./$RNSBOX_PACKER" "$VARIANT" ${RNSBOX_PACKER_BOARDS:-}
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
    sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
        buildroot/configs/cvitek_SG200X_musl_riscv64_defconfig
    if [ -f buildroot/.config ]; then
        sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
            buildroot/.config
    fi
    if [ -f buildroot/output/.config ]; then
        sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
            buildroot/output/.config
    fi
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
    # change between runs.
    # The stamp carries a fingerprint of exactly the sources the heavy
    # phases consume: u-boot, FSBL, the build driver scripts and the
    # per-board dts/defconfigs under build/. Rootfs-only changes
    # (S-scripts, wifi/rnsh conf, portal, zsh) leave the fingerprint
    # untouched and go straight to the pack stage - minutes instead of
    # a full kernel round. A changed boot source re-runs the phases
    # automatically: a bare touch-stamp once shipped a boot.sd with a
    # weeks-old u-boot after the radio bring-up patch landed.
    # git ls-tree hashes the INDEX (committed, reproducible) state, not
    # the untracked build outputs living under build/<board>/.
    local stage fp
    fp="$(git ls-tree -r HEAD -- build fsbl u-boot-2021.10 osdrv linux_5.10 buildroot/configs buildroot/package 2>/dev/null | sha256sum | cut -d' ' -f1)"
    [ -n "$fp" ] && [ "$fp" != "$(printf '' | sha256sum | cut -d' ' -f1)" ] || fp="no-sources"
    echo ">> stage fingerprint: $fp"
    for stage in build_uboot build_kernel build_ramboot build_osdrv \
                 build_3rd_party build_middleware pack_cfg; do
        if [ "$(cat ".rnsbox-done-$stage" 2>/dev/null)" != "$fp" ]; then
            "$stage" || return 1
            printf '%s\n' "$fp" > ".rnsbox-done-$stage"
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
    # lite shape, same as every RNSBox lite image: 128M rootfs ext4 in a
    # 1.6G partition (S99resizefs grows it on first boot), NCM-only gadget.
    sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
        buildroot/configs/cvitek_SG200X_musl_riscv64_defconfig
    if [ -f buildroot/.config ]; then
        sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
            buildroot/.config
    fi
    if [ -f buildroot/output/.config ]; then
        sed -i 's/^BR2_TARGET_ROOTFS_EXT2_SIZE=.*/BR2_TARGET_ROOTFS_EXT2_SIZE="512M"/' \
            buildroot/output/.config
    fi
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

    # Same self-invalidating fingerprint as the sg2000-rv64 flow: stamp
    # the boot-source state (u-boot, FSBL, build/ board dirs), so rootfs
    # edits skip the heavy phases while a boot patch re-runs them.
    local fp
    fp="$(git ls-tree -r HEAD -- build fsbl u-boot-2021.10 osdrv linux_5.10 buildroot/configs buildroot/package 2>/dev/null | sha256sum | cut -d' ' -f1)"
    [ -n "$fp" ] || fp="no-sources"
    for stage in build_uboot build_kernel build_ramboot build_osdrv \
                 build_3rd_party build_middleware pack_cfg; do
        [ "$(cat ".rnsbox-done-$tag-$stage" 2>/dev/null)" = "$fp" ] && continue
        "$stage" || return 1
        printf '%s\n' "$fp" > ".rnsbox-done-$tag-$stage"
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


rnsbox_build_rpi_universal() {    # Set up by the external flow below: $UPSTREAM (pristine clone at the
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

rnsbox_build_sg2000_duo_s_arm64_mainline() {
    # External flow, lite-only (one NCM gadget; the dvd clients LUN is not
    # wired for this board). Same shape as milkv-duo-cv1800b-mainline.
    export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    export PATH=/usr/sbin:/sbin:$PATH
    if [ "$VARIANT" != lite ]; then
        echo ">> sg2000-duo-s-arm64-mainline: lite only, building lite"
        VARIANT=lite
    fi
    export RNSBOX_VARIANT="$VARIANT"

    echo ">> configuring ($DEFCONFIG) + building (first run compiles everything; be patient)"
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT" "$DEFCONFIG"
    rnsbox_pin_dl
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT"
}

rnsbox_build_orangepi_r1() {
    # External flow (same shared upstream clone/dl cache as rpi-universal).
    # The H2+ port is lite-only: one NCM gadget, no clients DVD.
    export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    export PATH=/usr/sbin:/sbin:$PATH
    if [ "$VARIANT" != lite ]; then
        echo ">> orangepi-r1: the '$VARIANT' variant is not built (lite only)"
        VARIANT=lite
    fi
    export RNSBOX_VARIANT="$VARIANT"

    echo ">> configuring ($DEFCONFIG) + building (first run compiles everything; be patient)"
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT" "$DEFCONFIG"
    rnsbox_pin_dl
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT"
}

rnsbox_build_pc_x86_64() {
    # Same external flow as the rpi-universal (set up by the block below: $UPSTREAM
    # pristine clone, $EXT tree, $ODIR, pinned dl cache). No variant staging:
    # the image is tagged lite by board/pc/rnsbox-x86_64/post-image.sh; a dvd
    # request degrades to lite (no gadget, no clients LUN in a VM).
    export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    export PATH=/usr/sbin:/sbin:$PATH
    if [ "$VARIANT" != lite ]; then
        echo ">> pc-x86_64: the '$VARIANT' variant needs the USB gadget — building lite"
        VARIANT=lite
    fi
    export RNSBOX_VARIANT="$VARIANT"

    echo ">> configuring ($DEFCONFIG) + building (first run compiles everything; be patient)"
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT" "$DEFCONFIG"
    rnsbox_pin_dl
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT"
}

rnsbox_build_milkv_duo_cv1800b_mainline() {
    # Same external flow as the pc-x86_64 (defconfig + make; the image is
    # assembled by board/milkv-duo/post-image.sh inside make: vendor fip.bin
    # + our boot.sd FIT around the mainline Image/DTB). No dvd variant: no
    # USB clients LUN, and the gadget role itself is still being mainlined.
    export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/[a-z]/' | paste -sd:)
    export PATH=/usr/sbin:/sbin:$PATH
    if [ "$VARIANT" != lite ]; then
        echo ">> milkv-duo-cv1800b-mainline: the '$VARIANT' variant needs the USB gadget - building lite"
        VARIANT=lite
    fi
    export RNSBOX_VARIANT="$VARIANT"

    echo ">> configuring ($DEFCONFIG) + building (first run compiles everything; be patient)"
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT" "$DEFCONFIG"
    rnsbox_pin_dl
    make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT"
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

rnsbox_build() {
    case "$BOARD" in
        sg2002-rv64)      rnsbox_build_licheerv_nano ;;
        sg2000-arm64|sg2002-arm64|sg2002-licheerv-arm64) rnsbox_build_milkv_duo_01_arm ;;
        sg2000-rv64)      rnsbox_build_milkv_duo_01 ;;
        milkv-duo-cv1800b)     rnsbox_build_milkv_duo_cv1800b ;;
        rpi-universal)    rnsbox_build_rpi_universal ;;
        orangepi-r1)      rnsbox_build_orangepi_r1 ;;
        pc-x86_64)        rnsbox_build_pc_x86_64 ;;
        milkv-duo-cv1800b-mainline) rnsbox_build_milkv_duo_cv1800b_mainline ;;
        sg2000-duo-s-arm64-mainline) rnsbox_build_sg2000_duo_s_arm64_mainline ;;
        sg2000-duomodule01-mainline|sg2002-licheerv-nano-mainline|sg2002-duo256m-mainline|sg2000-duo-s-mainline) rnsbox_build_external ;;
    esac
}

# ---- external-tree flow (rpi-universal) -------------------------------------------
if [ "${BUILD_STYLE:-patches}" = external ]; then
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
    # from $EXT. Same rule as the patch-series flow below. aic8800 is the
    # out-of-tree WiFi driver source shipped in board/sg2000-mainline/.
    # hostapd: board/common/buildroot-patches/hostapd patches the nl80211
    # driver (monitor-less AP mgmt frames), which is only applied at extract.
    for p in rnsbox-update rnsbox-portal python-rns python-lxmf python-nomadnet python-ble-reticulum rnsbox-python-bluezero aic8800 hostapd; do
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

    rnsbox_build

    echo ">> done — image(s) under $ODIR/images/"
    ls -lh "$ODIR/images/rnsbox-$BOARD.img" "$ODIR/images/rnsbox-$BOARD-$VARIANT.img" 2>/dev/null | tail -2 || true
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

# sudo refuses a sudoers file it can write; git cannot carry the 0440
# file mode, so enforce it on the overlay copy at every build (fresh
# clone and reuse alike). NB: if/fi, not an AND-list — under set -e a
# false test here would kill the script.
if [ -e "$WORKDIR/buildroot/board/cvitek/SG200X/overlay/etc/sudoers" ]; then
    chmod 0440 "$WORKDIR/buildroot/board/cvitek/SG200X/overlay/etc/sudoers"
fi

# Shared packages live in ONE place — <repo>/package/ — no matter how many
# boards this repo grows to. The patch series deliberately does not vendor
# them (its last patch deletes the copies the early series added); copy the
# current shared versions into the BSP buildroot before configuring.
echo ">> syncing shared packages from $HERE/package"
for p in rnsbox-update rnsbox-portal python-rns python-lxmf python-nomadnet python-ble-reticulum rnsbox-python-bluezero; do    rm -rf "$WORKDIR/buildroot/package/$p"
    cp -a "$HERE/package/$p" "$WORKDIR/buildroot/package/$p"
done
# The un-vendoring BSP patch removed the matching `source` lines from
# buildroot/package/Config.in together with the vendored copies; without
# them kconfig silently drops BR2_PACKAGE_PYTHON_RNS / PYTHON_LXMF /
# PYTHON_NOMADNET / RNSBOX_PORTAL and the rootfs ships without rnsd and
# the portal. Restore the lines so the synced dirs are visible.
BR_PKG_CONFIG="$WORKDIR/buildroot/package/Config.in"
for p in rnsbox-update rnsbox-portal python-rns python-lxmf python-nomadnet python-ble-reticulum rnsbox-python-bluezero; do    grep -q "source \"package/$p/Config.in\"" "$BR_PKG_CONFIG" || \
        printf 'source "package/%s/Config.in"\n' "$p" >> "$BR_PKG_CONFIG"
done

# These are SITE_METHOD=local packages: buildroot rsyncs them into
# output/build/<pkg>-<ver> ONCE at extract time and never again — an
# incremental build happily recompiles a STALE copy (this shipped a
# portal binary with week-old sources). The synced sources above are the
# single source of truth, so drop the extracted copies and their stamps;
# the next make re-extracts and rebuilds exactly what this repo carries.
# Small packages (portal C++, pure-python wheels): seconds on a warm host.
for p in rnsbox-update rnsbox-portal python-rns python-lxmf python-nomadnet python-ble-reticulum rnsbox-python-bluezero; do
    rm -rf "$WORKDIR"/buildroot/output/build/"$p"-* 2>/dev/null || :
done

# rootfs slimming: sync the slimmer + flip the size/format knobs (see the
# rnsbox_slim_bsp comment block above for what and why)
echo ">> slimming knobs: py-only python, 1% ext4 reservation, 512M lite rootfs, slim-target hook"
rnsbox_slim_bsp "$WORKDIR"

# unicodedata (idna codec) lands only when python3 RE-configures: a warm
# output/target built before the flip keeps missing lib-dynload/
# unicodedata.so no matter what .config says - dirclean python3 once, the
# next make rebuilds it with --enable-unicodedata. No-op once the target
# carries the extension.
if grep -q '^BR2_PACKAGE_PYTHON3_UNICODEDATA=y' "$WORKDIR/buildroot/.config" 2>/dev/null \
   && ! ls "$WORKDIR"/buildroot/output/target/usr/lib/python3.*/lib-dynload/unicodedata*.so >/dev/null 2>&1; then
    echo ">> python3: unicodedata newly enabled - dircleaning python3 for a rebuild"
    rm -rf "$WORKDIR"/buildroot/output/build/python3-*
fi

rnsbox_track_latest_rns "$PWD"
rnsbox_build

echo ">> done — image(s) under $WORKDIR/$RNSBOX_IMAGES_DIR/"
for d in $RNSBOX_IMAGES_DIR; do
    ls -lh "$WORKDIR/$d"/*-"$VARIANT".img 2>/dev/null | tail -2 || true
done
