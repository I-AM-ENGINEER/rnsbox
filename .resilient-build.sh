#!/bin/bash
# Crash-resilient build loop for the rnsbox mainline flow: the WSL VM on this
# host resets under load (killing builds mid-package); buildroot's incremental
# state survives, only the package that was mid-build gets corrupted. On each
# failure: remove the package dir the error names, rerun. Up to 10 attempts.
set -u
O=/home/noname/rnsbox-work/upstream/out/milkv-duo-cv1800b-mainline
LOG=/home/noname/rnsbox-work/logs/mainline-build-loop.log
cd /home/noname/rnsbox-work/rnsbox-mainline || exit 90

for i in 1 2 3 4 5 6 7 8 9 10; do
    echo "=== attempt $i $(date -u +%H:%M:%S) ===" >> "$LOG"
    UPSTREAM_BASE=/home/noname/rnsbox-work/upstream RNSBOX_NPROC=2 \
        ./build.sh milkv-duo-cv1800b-mainline lite >> "$LOG" 2>&1
    rc=$?
    echo "=== attempt $i exit=$rc $(date -u +%H:%M:%S) ===" >> "$LOG"
    [ $rc -eq 0 ] && { echo "BUILD-LOOP-OK" >> "$LOG"; exit 0; }

    # find the package dir the failure names (pkg-generic stamp error)
    broken=$(grep -oE "$O/build/[a-zA-Z0-9._+-]+" "$LOG" | tail -1)
    if [ -n "$broken" ] && [ -d "$broken" ]; then
        echo "=== cleaning broken: $broken ===" >> "$LOG"
        rm -rf "$broken"
    else
        echo "=== no broken package dir identified; stopping ===" >> "$LOG"
        exit 1
    fi
done
echo "BUILD-LOOP-GAVE-UP" >> "$LOG"
exit 2
