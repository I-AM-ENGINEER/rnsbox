#!/bin/bash
# gen-patches.sh — build the git-am patches for the rnsbox BSP series.
# Uses a scratch git repo seeded with the final-state files (extracted from
# the warm arm64 workspace), applies the fixes, and emits real
# git-format-patch output (correct headers, modes, new-file handling).
set -eu

REPO=/home/noname/init-fix
WS=/home/noname/rnsbox/milkv-duo-01-arm   # warm arm64 workspace (licheerv+arm series applied)
SCRATCH=/tmp/rnsbox-scratch
export GIT_AUTHOR_NAME=rnsbox GIT_AUTHOR_EMAIL=rnsbox@localhost
export GIT_COMMITTER_NAME=rnsbox GIT_COMMITTER_EMAIL=rnsbox@localhost
export GIT_AUTHOR_DATE="2026-10-17T00:00:00+03:00"
export GIT_COMMITTER_DATE="$GIT_AUTHOR_DATE"

# seed <path> — copy the workspace's final-state file into the scratch tree
seed() {
    mkdir -p "$SCRATCH/tree/$(dirname "$1")"
    cp "$WS/$1" "$SCRATCH/tree/$1"
}

commit() { # $1 subject, $2 body
    git -C "$SCRATCH/tree" add -A
    git -C "$SCRATCH/tree" commit -qm "$1

$2"
}

out_patch() { # $1 output file
    git -C "$SCRATCH/tree" format-patch -1 --stdout > "$1"
    # reset base for the next patch: commit the tree state and continue
}

rm -rf "$SCRATCH"; mkdir -p "$SCRATCH/tree"
git -C "$SCRATCH/tree" init -q
git -C "$SCRATCH/tree" config user.name rnsbox
git -C "$SCRATCH/tree" config user.email rnsbox@localhost

############################################################################
# Patch A: defer the boot-time first dnsmasq start to S80dnsmasq
# (lickeerv 0031 / milkv-duo-01 0036)
############################################################################
F=buildroot/board/cvitek/SG200X/overlay/etc/init.d/S60routing
seed "$F"
git -C "$SCRATCH/tree" add -A && git -C "$SCRATCH/tree" commit -qm base
python3 - "$SCRATCH/tree/$F" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = """\tif [ -x /etc/init.d/S80dnsmasq ]; then
\t\t/etc/init.d/S80dnsmasq restart >/dev/null 2>&1 || true
\telif pidof dnsmasq >/dev/null 2>&1; then
\t\tkillall -HUP dnsmasq 2>/dev/null || true
\tfi
"""
new = """\t# Restart dnsmasq ONLY while it is already running. At boot S60routing
\t# runs BEFORE rcS reaches S80dnsmasq: a restart here silently STARTS
\t# dnsmasq early, and S80's own start then fails noisily with
\t# "Starting dnsmasq: /usr/sbin/dnsmasq is already running FAIL" — the
\t# double start in every boot log. When it is not running yet, S80dnsmasq
\t# starts it moments later with the regenerated scope below.
\tif pidof dnsmasq >/dev/null 2>&1; then
\t\tif [ -x /etc/init.d/S80dnsmasq ]; then
\t\t\t/etc/init.d/S80dnsmasq restart >/dev/null 2>&1 || true
\t\telse
\t\t\tkillall -HUP dnsmasq 2>/dev/null || true
\t\tfi
\tfi
"""
assert old in s, "dnsmasq block not found in S60routing"
open(p, "w").write(s.replace(old, new, 1))
print("S60routing patched")
PYEOF
commit "router: defer the first dnsmasq start to S80dnsmasq" \
"S60routing's apply() restarted dnsmasq unconditionally. At boot that is
the FIRST start (rcS has not reached S80dnsmasq yet), so S80's own start
then fails with \"already running FAIL\" on every boot. Restart only
while it is already running; the boot-time start belongs to S80dnsmasq,
which runs right after with the regenerated per-LAN scope."
out_patch "$REPO/patches/licheerv-nano/0031-router-defer-the-first-dnsmasq-start-to-S80.patch"
cp "$REPO/patches/licheerv-nano/0031-router-defer-the-first-dnsmasq-start-to-S80.patch" \
   "$REPO/patches/milkv-duo-01/0036-router-defer-the-first-dnsmasq-start-to-S80.patch"
echo "wrote licheerv-nano/0031 + milkv-duo-01/0036 (dnsmasq defer)"

############################################################################
# Patch B: rnsd outage flag cleared on every successful start
# (licheerv 0032 / milkv-duo-01 0037)
############################################################################
F1=buildroot/board/cvitek/SG200X/overlay/etc/init.d/S82rnsd
F2=buildroot/board/cvitek/SG200X/overlay/etc/init.d/S46wanwatch
seed "$F1"; seed "$F2"
git -C "$SCRATCH/tree" add -A && git -C "$SCRATCH/tree" commit -qm base
python3 - "$SCRATCH/tree/$F1" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = """\tif kill -0 "$PID" 2>/dev/null; then
\t\techo "OK (pid $PID)"
"""
new = """\tif kill -0 "$PID" 2>/dev/null; then
\t\t# A successful start is THE authoritative end of any outage: clear
\t\t# the wanwatch down-streak flag so the portal never shows a stale
\t\t# "rnsd DOWN" badge next to a live pid (previously only wanwatch's
\t\t# next 10 s tick cleared it — which a dead wanwatch never did, and
\t\t# a portal-initiated restart left the flag behind entirely).
\t\trm -f /run/rnsd-down-since
\t\techo "OK (pid $PID)"
"""
assert old in s, "start-success block not found in S82rnsd"
open(p, "w").write(s.replace(old, new, 1))
print("S82rnsd patched")
PYEOF
python3 - "$SCRATCH/tree/$F2" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = """\t\tif [ -z "$countable" ] \t\t   || { [ -f "$RNSD_PIDFILE" ] && kill -0 "$(cat "$RNSD_PIDFILE")" 2>/dev/null; } \t\t   || pidof -x rnsd >/dev/null 2>&1; then
\t\t\trm -f /run/rnsd-down-since
"""
new = """\t\tif [ -z "$countable" ] \t\t   || { [ -f "$RNSD_PIDFILE" ] && kill -0 "$(cat "$RNSD_PIDFILE")" 2>/dev/null; } \t\t   || pidof -x rnsd >/dev/null 2>&1; then
\t\t\tif [ -f /run/rnsd-down-since ]; then
\t\t\t\tsince=$(cat /run/rnsd-down-since 2>/dev/null)
\t\t\t\tlog "rnsd back up — outage streak since """ + "${since:-?}" + """s cleared"
\t\t\t\trm -f /run/rnsd-down-since
\t\t\tfi
"""
assert old in s, "down-flag clear block not found in S46wanwatch"
open(p, "w").write(s.replace(old, new, 1))
print("S46wanwatch patched")
PYEOF
commit "rnsd: clear the outage flag on every successful start" \
"The portal's rnsd DOWN badge reads /run/rnsd-down-since, which only
wanwatch's 10 s tick cleared. A portal-initiated restart (or a dead
wanwatch) left the flag behind: the portal kept showing \"rnsd DOWN n
min\" next to a live pid. S82rnsd now clears the flag on every
successful start (boot, wanwatch kick, portal Restart) and wanwatch
logs the recovery instead of clearing silently."
out_patch "$REPO/patches/licheerv-nano/0032-rnsd-clear-the-outage-flag-on-every-successful-start.patch"
cp "$REPO/patches/licheerv-nano/0032-rnsd-clear-the-outage-flag-on-every-successful-start.patch" \
   "$REPO/patches/milkv-duo-01/0037-rnsd-clear-the-outage-flag-on-every-successful-start.patch"
echo "wrote licheerv-nano/0032 + milkv-duo-01/0037 (rnsd outage flag)"

echo "PHASE1-DONE"
