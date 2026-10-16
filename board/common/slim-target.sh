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

# ---- aic8800 WiFi firmware: keep only the plain-8800 family ---------------
# The aic8800_bsp module compiles in CONFIG_AIC_FW_PATH pointing at
# aic8800_sdio/aic8800_and_aic8800D80 — the aic8800DC/ and aic8800D80X2/
# siblings are never referenced, and inside the main folder only the
# chipid-8800 names (fmacfw*, fw_patch*, fw_adid*, aic_userconfig*) load on
# the SDIO radio of the -W boards; the *_8800d80* names belong to the D80
# USB-dongle silicon (different SDIO chipid, absent on RNSBox boards).
FW="$T/usr/lib/firmware/aic8800_sdio"
rm -rf "$FW/aic8800DC" "$FW/aic8800D80X2"
rm -f "$FW"/aic8800_and_aic8800D80/*8800d80* "$FW"/aic8800_and_aic8800D80/*8800D80*

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
