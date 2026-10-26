#!/bin/bash
# check-overlap.sh — keep the rootfs overlays free of accidental duplicates.
#
#   scripts/check-overlap.sh           check the tree
#
# board/common/rootfs-overlay is listed ahead of every board overlay in
# BR2_ROOTFS_OVERLAY, so it is the single home for anything shared: a board
# overlay only carries a file when its copy deliberately differs (the
# Buildroot later-wins rule makes that override win). Two board overlays
# shipping a byte-identical file - or a board overlay shipping a file
# byte-identical to the common overlay's - is therefore always a copy that
# forgot to move home, and this check fails loudly on it, the same way
# gen-defconfigs.sh --check fails on a stale defconfig.
#
# Byte-different files at the same path are fine: that is the override
# mechanism working as designed. So are the DELIBERATE subset duplicates in
# allowed() below: files carried only by the boards whose hardware or feature
# set wants them. A single common overlay ships to every board, so hosting a
# subset file there would force it onto boards that never had it (S83nomadnet
# would start the NomadNet daemon on ht-hd01-v2-core, an fstab from another
# board would corrupt partition mounts). Those stay as per-board copies.
set -eu

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
COMMON="$ROOT/board/common/rootfs-overlay"

# allowed <relative-path> — true when identical copies between a subset of
# board overlays are deliberate. Keep the inline reason next to each entry.
allowed() {
    case "$1" in
        # ---- boards without the ht-hd01-v2 service stack ------------------
        etc/dhcpcd.conf|etc/dhcpcd.exit-hook)   return 0 ;; # no dhcpcd on ht
        etc/init.d/S09ipv6|etc/init.d/S45ntpsync|etc/init.d/S46wanwatch) return 0 ;;
        etc/init.d/S83nomadnet|etc/init.d/S85lxmd) return 0 ;; # nomadnet/lxmd daemons, ht-core ships neither
        etc/init.d/rcS)                         return 0 ;; # ht uses the skeleton rcS
        etc/lxmd/config|etc/nftables.conf)      return 0 ;;
        etc/sysctl.d/20-rnsbox-net.conf|etc/sysctl.d/30-rnsbox-panic.conf) return 0 ;;
        # ---- embedded-SBC set (cpufreq/zram/usb-gadget/wifi watch) --------
        etc/init.d/S04cpufreq|etc/init.d/S05zram|etc/init.d/S06fstrim) return 0 ;;
        etc/init.d/S10usbdev|etc/init.d/S30gadget_nic|etc/init.d/S36wifiwatch) return 0 ;;
        # ---- full-featured pair and its complements (two variants each) ---
        etc/init.d/S02resizefs|etc/init.d/S03data|etc/init.d/S31slip) return 0 ;; # opi+rpi variant
        etc/init.d/S37bluetooth|etc/init.d/S98confirm) return 0 ;; # BLE stack boards
        etc/bluetooth/main.conf|etc/default/bluetoothd) return 0 ;;
        etc/init.d/S84cron|etc/init.d/S86rnsh|etc/ssh/sshd_config|etc/sysctl.d/10-rnsbox-hardening.conf) return 0 ;; # two board sets
        etc/init.d/S60routing)                  return 0 ;; # mil/pc/sg2k variant; opi/rpi and ht ship their own
        etc/init.d/S35wifi)                     return 0 ;; # per-board WiFi bring-up; mil/rpi happen to match
        etc/init.d/S03watchdog)                 return 0 ;; # mil/rpi/sg2k hardware watchdog
        # ---- gated SSH (stock openssh starter on mil/pc, own on ht) -------
        etc/init.d/S50sshd)                     return 0 ;;
        # ---- echobot feature set -------------------------------------------
        etc/init.d/S87echobot|etc/rnsbox/echobot.conf|etc/rnsbox/ssh.conf|usr/bin/rnsbox-echobot) return 0 ;;
        # ---- genuinely per-board content ------------------------------------
        etc/fstab)                              return 0 ;; # partition layout; identical bytes are coincidence
        etc/inittab)                            return 0 ;; # console/boot wiring
        etc/securetty)                          return 0 ;;
        usr/share/udhcpc/default.script)        return 0 ;;
        etc/rnsbox/nomadnet.conf)               return 0 ;; # ht+mil outlier pair (common ships the majority default)
        etc/rnsbox/slip.conf)                   return 0 ;; # per-board SLIP modem wiring; ht/opi happen to match
        *)                                      return 1 ;;
    esac
}

[ -d "$COMMON" ] || { echo "FATAL: missing $COMMON" >&2; exit 1; }

mapfile -t OVERLAYS < <(find "$ROOT/board" -type d \
    \( -name rootfs-overlay -o -name full-overlay \) | sort)

name() { basename "$(dirname "$1")"; }

files() {
    (cd "$1" && find . -type f | sed 's|^\./||' | sort)
}

fail=0

# check_pair <dir-a> <dir-b> <message prefix>: every relative path present in
# both overlays that is byte-identical is a violation (unless allowlisted).
check_pair() {
    local a="$1" b="$2" prefix="$3" f
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        allowed "$f" && continue
        if cmp -s "$a/$f" "$b/$f"; then
            echo "error: $prefix: $f" >&2
            fail=1
        fi
    done <<< "$(comm -12 <(files "$a") <(files "$b"))"
}

for ov in "${OVERLAYS[@]}"; do
    [ "$ov" = "$COMMON" ] && continue
    check_pair "$COMMON" "$ov" \
        "$(name "$ov") duplicates the common overlay — delete the board copy:"
done

for ((i = 0; i < ${#OVERLAYS[@]}; i++)); do
    for ((j = i + 1; j < ${#OVERLAYS[@]}; j++)); do
        a="${OVERLAYS[i]}" b="${OVERLAYS[j]}"
        if [ "$a" = "$COMMON" ] || [ "$b" = "$COMMON" ]; then continue; fi
        check_pair "$a" "$b" \
            "$(name "$a") and $(name "$b") ship identical — move to board/common/rootfs-overlay:"
    done
done

if [ "$fail" -ne 0 ]; then
    echo "check-overlap: duplicate overlay files (see above)" >&2
    exit 1
fi
echo "check-overlap: no duplicate files across overlays"
