#!/bin/sh
# slim-target.sh — delete dead weight from the buildroot TARGET_DIR before
# the rootfs image is made. Runs as a BR2_ROOTFS_POST_BUILD_SCRIPT on every
# RNSBox board flow (CVITEK BSP flow wires it up in build.sh, the
# rpi-universal external flow appends it to BR2_ROOTFS_POST_BUILD_SCRIPT in
# the defconfig).
# buildroot calls each post-build script with the target dir as $1.
#
# Only deletion of things that are PROVABLY unused happens here; everything
# is guarded so the script is idempotent and never fails a build on a tree
# where an item is absent (rm -f/rm -rf of a missing path is a no-op).
#
# What stays out on purpose (would break shipped functionality):
#   pip/setuptools/pkg_resources — the portal "Update rnsd" button and
#     on-device package management (patches 0018/0019 of the BSP series);
#   stdlib distutils — setuptools' distutils-precedence hook and on-device
#     sdist builds still reach for it;
#   unittest, encodings, lib-dynload — small, and some tooling imports them;
#   brcm/cypress WiFi firmware — real Pi radios; aic8800 plain-8800 files —
#     the radio of every -W board;
#   USB-Ethernet ko family in /mnt/system/ko — dongle support.
#
# Verified by import grep at image-size time: nothing under
# site-packages/{RNS,LXMF,nomadnet} imports pkg_resources/setuptools/pip/
# distutils/ensurepip/idlelib/turtle/pydoc/lib2to3/unittest, and the portal
# is C++. Details: local_docs/image-size-reduction.md.
set -eu
T="${1:-.}"
[ -d "$T/usr" ] || { echo "slim-target: '$T' does not look like a TARGET_DIR" >&2; exit 1; }

# ---- boot mountpoint and overlay exec bits --------------------------------
# The buildroot skeleton has no /boot, but every RNSBox fstab mounts the
# boot FAT there (the usb.* gadget flag files live on it). Without the
# directory the sysinit mount fails and USB gadget setup has no flags.
mkdir -p "$T/boot"

# Stable per-device hostname (rnsbox-XXXXXX from the best stable MAC, or a
# persisted random ID when every radio hands out a random address). Shared by
# every board that runs this post-build script.
COMMON_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
if [ -f "$COMMON_DIR/S10hostname" ] && [ -d "$T/etc/init.d" ]; then
	install -m 0755 "$COMMON_DIR/S10hostname" "$T/etc/init.d/S10hostname"
fi

# When the repo is checked out on NTFS/DrvFs (WSL), every file is reported
# as 0644, so rsync -a cannot carry the executable bit of the overlay init
# scripts. busybox init execve()s rcS directly: 0644 there means the box
# boots to a login prompt with no services ("can't run rcS: Permission
# denied"). build.sh re-applies the git modes to the WSL-side copy; repeat
# the critical ones here so the image is correct regardless of how the
# overlay was populated.
if [ -d "$T/etc/init.d" ]; then
	chmod 0755 "$T/etc/init.d/rcS" "$T/etc/init.d/rcK" 2>/dev/null || true
	for s in "$T"/etc/init.d/S*; do
		if [ -e "$s" ]; then chmod 0755 "$s"; fi
	done
fi
if [ -e "$T/usr/share/udhcpc/default.script" ]; then
	chmod 0755 "$T/usr/share/udhcpc/default.script"
fi

# ---- python: stdlib modules a headless router never imports ---------------
# ensurepip (~4M): pip is preinstalled from the rootfs packages; ensurepip
#   only bootstraps pip into a venv.
# idlelib, turtledemo, turtle, pydoc_data, lib2to3: GUI/teaching/doc tooling.
# stdlib test suite: not shipped to production targets.
for d in ensurepip idlelib turtledemo pydoc_data lib2to3 test; do
	rm -rf "$T"/usr/lib/python3.*/"$d"
done
rm -f "$T"/usr/lib/python3.*/turtle.py*
# site-packages test suites (keep the packages themselves)
find "$T"/usr/lib/python3.*/site-packages -type d -name tests -prune -exec rm -rf {} + 2>/dev/null || true

# ---- aic8800 WiFi firmware: keep only the chip families the -W radio uses -
# The aic8800_bsp module compiles in CONFIG_AIC_FW_PATH pointing at
# aic8800_sdio/aic8800_and_aic8800D80 — the aic8800DC/ and aic8800D80X2/
# sibling DIRECTORIES are never referenced. Inside the main folder, the
# radio of the -W boards is an AIC8800D80 (SDIO vid 0xc8a1 did 0x0082,
# proven live): it boots the *_8800d80_u02 firmware set (fmacfwbt + BT
# patches), so those names MUST be kept — an earlier revision of this
# script deleted them as "USB-dongle silicon" and the radio could not
# even boot WiFi. The plain-8800 names (fmacfw.bin/fmacfwbt.bin/…) stay
# too: the same bsp requests them for the AIC8801 chipid.
FW="$T/usr/lib/firmware/aic8800_sdio"
rm -rf "$FW/aic8800DC" "$FW/aic8800D80X2"

# ---- vendor ko stash: modules nothing loads -------------------------------
# S00kmod insmods soph_* (kernel-built here), S35wifi finds cfg80211/rfkill/
# aic8800_{bsp,fdrv}; no script ever loads these:
#   hynitron touchscreens, gspca webcam core, display backlight/PWM,
#   efivarfs (no EFI on the CVitek boards), alternative TCP congestion
#   algorithms (cubic is the built-in default).
for k in hynitron_touch.ko hynitron_touch_new.ko gspca_main.ko \
         backlight.ko pwm_bl.ko efivarfs.ko \
         tcp_bic.ko tcp_htcp.ko tcp_westwood.ko; do
	rm -f "$T/mnt/system/ko/$k"
done

# ---- tzdata: nothing reads it ---------------------------------------------
# No /etc/TZ, no timezone setting in the portal or the init chain; the box
# runs UTC and logs UTC. Python's zoneinfo module falls back to tzdata pip
# package if a user ever sets one up.
rm -rf "$T/usr/share/zoneinfo"

# ---- misc single-purpose leftovers ----------------------------------------
rm -f "$T/usr/share/pixmaps/htop.png"   # htop runs fine iconless; ~1M X11 icon

echo "slim-target: dead weight removed"
