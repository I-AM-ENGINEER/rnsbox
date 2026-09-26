#!/bin/bash
# Build the read-only, pre-populated ISO9660 "DVD" client image from whatever
# binaries fetch-clients.sh placed under dl/clients/<App>/ (version-agnostic --
# no filenames are hardcoded, so bumping to newer upstream releases needs no
# change here). Emulated as an optical disc by f_mass_storage (lun.0/cdrom=1),
# so the host treats it as read-only media and never offers to format it.
#
# xorriso flags: -J Joliet (Windows long names), -r Rock Ridge (Unix names +
# perms), -V volume id RNSBOX. Output ships inside rootfs at
# /opt/rnsbox-clients/usbdisk.iso; S10usbdev points the mass-storage LUN at it.
set -u
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$ROOT" || exit 90
SRC="$ROOT/dl/clients"
EXTRA="$ROOT/clients-stick/extra"
OUTDIR="$ROOT/board/raspberrypi/rnsbox-0-2w/rootfs-overlay/opt/rnsbox-clients"
OUTISO="$OUTDIR/usbdisk.iso"
LABEL="RNSBOX"
APPS="MeshChat MeshChatX Ratspeak Columba Sideband"
FIND_BINS=( -type f \( -name '*.exe' -o -name '*.msi' -o -name '*.dmg' \
	-o -name '*.AppImage' -o -name '*.appimage' -o -name '*.deb' -o -name '*.rpm' \
	-o -name '*.apk' -o -name '*.zip' -o -name '*.flatpak' \) )

echo "=== build-clients-img (ISO9660) $(date) ==="
command -v xorriso >/dev/null 2>&1 || { echo "FATAL: xorriso not on host (apt-get install xorriso)"; exit 91; }

miss=0
for a in $APPS; do
	n=$(find "$SRC/$a" "${FIND_BINS[@]}" 2>/dev/null | wc -l)
	echo "  $a: $n binaries"
	[ "$n" -ge 1 ] || { echo "    MISSING binaries for $a"; miss=1; }
done
[ "$miss" = 0 ] || { echo "FATAL: missing client binaries (run ./fetch-clients.sh first)"; exit 92; }

STAGE=$(mktemp -d)
for a in $APPS; do
	mkdir -p "$STAGE/$a"
	find "$SRC/$a" "${FIND_BINS[@]}" -exec cp {} "$STAGE/$a/" \;
done
[ -f "$SRC/MANIFEST.txt" ] && cp "$SRC/MANIFEST.txt" "$STAGE/SHA256SUMS.txt"
cp "$EXTRA/README.txt" "$EXTRA/index.html" "$STAGE/" 2>/dev/null || true

echo "  staged $(du -sh "$STAGE" | cut -f1) across $APPS"

mkdir -p "$OUTDIR"
rm -f "$OUTISO"
xorriso -as mkisofs \
	-V "$LABEL" \
	-J -joliet-long -r \
	-o "$OUTISO" \
	"$STAGE" 2>/tmp/xorriso.log
rc=$?
if [ $rc -ne 0 ]; then echo "FATAL: xorriso rc=$rc"; tail -8 /tmp/xorriso.log; rm -rf "$STAGE"; exit 93; fi

echo "=== verify ISO ==="
echo "  size: $(du -h "$OUTISO" | cut -f1)"
file "$OUTISO" | sed 's/^/  /'
xorriso -indev "$OUTISO" -toc 2>/dev/null | grep -iE 'Volume id|Media' | sed 's/^/    /'
rm -rf "$STAGE"
echo "CLIENTS-IMG-DONE"
