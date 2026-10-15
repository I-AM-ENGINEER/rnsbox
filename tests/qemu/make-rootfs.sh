#!/bin/bash
# make-rootfs.sh — build the QEMU acceptance rootfs: a copy of a real RNSBox
# BSP buildroot output with the rnsbox-update package injected via debugfs
# (no mount, no root).
#
# usage: make-rootfs.sh <src-rootfs.ext4> <out-rootfs.ext4> <repo-root>
set -e
SRC="$1"; OUT="$2"; REPO="$3"
PKG="$REPO/package/rnsbox-update/src"

cp -f "$SRC" "$OUT"
# rootfs.ext2 images are sparse-ish; make sure we have headroom (file is
# fixed-size already, the BSP sizes it generously)

INJ=$(mktemp -d)
mkdir -p "$INJ/usr-lib" "$INJ/providers" "$INJ/etc"
cp "$PKG/rnsbox-sysupdate" "$INJ/usr-lib/rnsbox-sysupdate"
cp "$PKG/abselect" "$INJ/usr-lib/rnsbox-abselect"
cp "$PKG/update_providers/github_releases.py" "$INJ/providers/" 2>/dev/null || true
cp "$PKG/update_providers/generic.py" "$INJ/providers/" 2>/dev/null || true
cp "$PKG/update.conf" "$INJ/etc/update.conf"

# debugfs writes files with 0100644; fix exec bits afterwards with sif
dbg() { debugfs -w -f - "$OUT" >/dev/null 2>&1 <<< "$1"; }

dbg "rm /usr/sbin/rnsbox-sysupdate"
dbg "write $INJ/usr-lib/rnsbox-sysupdate /usr/sbin/rnsbox-sysupdate"
dbg "sif /usr/sbin/rnsbox-sysupdate mode 0100755"
dbg "mkdir /usr/lib/rnsbox"
dbg "rm /usr/lib/rnsbox/abselect"
dbg "write $INJ/usr-lib/rnsbox-abselect /usr/lib/rnsbox/abselect"
dbg "sif /usr/lib/rnsbox/abselect mode 0100755"
dbg "mkdir /usr/lib/rnsbox/update_providers"
for f in "$INJ/providers"/*.py; do
    b=$(basename "$f")
    dbg "rm /usr/lib/rnsbox/update_providers/$b"
    dbg "write $f /usr/lib/rnsbox/update_providers/$b"
done
dbg "rm /etc/rnsbox/update.conf"
dbg "write $INJ/etc/update.conf /etc/rnsbox/update.conf"

rm -rf "$INJ"
# sanity: the files are really in there
debugfs -R "cat /usr/lib/rnsbox/abselect" "$OUT" 2>/dev/null | head -1 | grep -q abselect
debugfs -R "cat /etc/rnsbox/update.conf" "$OUT" 2>/dev/null | grep -q "^enabled=yes"
echo "ROOTFS_OK $OUT"
