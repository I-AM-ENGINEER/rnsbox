#!/bin/bash
# run-rv.sh — QEMU acceptance orchestrator for rnsbox-sysupdate.
# Method: ~/rnsbox-work/duo01-rv-validate (real RNSBox rootfs + riscv64 virt
# kernel + expect). Usage: run-rv.sh [src-rootfs] [qemu-kernel]
set -o pipefail

REPO=/home/noname/rnsbox-work/rnsbox-update
SRC="${1:-/home/noname/rnsbox-work/duo01-bsp/buildroot/output/images/rootfs.ext4}"
KERNEL="${2:-/home/noname/rnsbox-work/upstream/out-qemu-rv/images/Image}"
WORK=$(mktemp -d /tmp/rnsbox-acc.XXXXXX)
PORT=8765

cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

[ -f "$SRC" ] || { echo "FATAL: no rootfs at $SRC"; exit 2; }
[ -f "$KERNEL" ] || { echo "FATAL: no kernel at $KERNEL"; exit 2; }

echo "=== 1. inject the rnsbox-update package into a rootfs copy"
"$REPO/tests/qemu/make-rootfs.sh" "$SRC" "$WORK/rootfs.ext4" "$REPO"

echo "=== 2. fake releases server on :$PORT"
python3 "$REPO/tests/qemu/fake-server.py" "$PORT" > "$WORK/server.log" 2>&1 &
SRV_PID=$!
for i in $(seq 1 20); do
    grep -q "FAKE SERVER READY" "$WORK/server.log" && break
    sleep 0.5
done
grep -q "FAKE SERVER READY" "$WORK/server.log" || { echo "FATAL: server"; cat "$WORK/server.log"; exit 3; }
ACC_SHA=$(sed -n "s/.*sha=\([0-9a-f]*\).*/\1/p" "$WORK/server.log" | head -1)
ACC_TAG=$(sed -n "s/.*tag=\([^ ]*\).*/\1/p" "$WORK/server.log" | head -1)
echo "    tag=$ACC_TAG sha=$ACC_SHA"

echo "=== 3. boot + acceptance"
QEMU_KERNEL="$KERNEL" QEMU_ROOTFS="$WORK/rootfs.ext4" \
    ACC_SHA="$ACC_SHA" ACC_TAG="$ACC_TAG" ACC_PORT="$PORT" \
    expect -f "$REPO/tests/qemu/accept-rv.expect" 2>&1 | tee "$WORK/acc.log"

echo "=== 4. summary"
PASS=$(grep -cE "WORKER-OK|PYC-OK|FIXTURE-OK|DEVMAP-OK|CONF-OK|ENV-OK|\"layout\": \"ab\"|\"phase\": \"available\"|$ACC_TAG|\"phase\": \"flipping\"|FLIPPED-TO-B|TRIES-RESET|SLOTB-CONTENT-OK|BOOTSD-STAGED|BOOTSD-BACKUP|CIMG-STRIPPED|\"phase\": \"applied\"|\"phase\": \"rolled_back\"|ABSELECT-ROLLBACK-OK|refusing non-HTTPS|SLOT-A-CLEAN" "$WORK/acc.log" || true)
echo "    acceptance markers: $PASS (expect >= 18)"
if [ "$PASS" -ge 18 ] && ! grep -q "TIMEOUT-mark" "$WORK/acc.log"; then
    echo "QEMU-ACCEPTANCE: PASS"
    exit 0
fi
echo "QEMU-ACCEPTANCE: FAIL — see $WORK/acc.log (kept: /tmp copy below)"
cp "$WORK/acc.log" /tmp/rnsbox-acc-last.log
exit 1
