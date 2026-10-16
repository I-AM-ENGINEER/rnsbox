#!/bin/bash
T=$(mktemp -d)
mkdir -p "$T/bsp/buildroot/configs"
touch "$T/bsp/buildroot/configs/cvitek_SG200X_64bit_defconfig"
echo "T=$T"
sed -n "/^# ---- rootfs slimming/,/^# ---- board build steps/p" \
    "$(dirname "$0")/../build.sh" | sed '$d' > /tmp/fns-dbg.sh
echo "--- glob line in extracted fns:"
grep -n "cvitek_" /tmp/fns-dbg.sh | head
echo "--- manual loop:"
for dc in "$T"/bsp/buildroot/configs/cvitek_*.defconfig; do
    [ -f "$dc" ] && echo "REAL FILE: $dc" || echo "LITERAL: $dc"
done
echo "--- source and call assert:"
source /tmp/fns-dbg.sh
echo "bsp arg will be: $T/bsp"
ls -la "$T/bsp/buildroot/configs/"