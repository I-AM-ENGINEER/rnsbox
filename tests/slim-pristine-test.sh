#!/bin/bash
# End-to-end proof: rnsbox_slim_bsp transforms the PRISTINE upstream
# cvitek defconfigs (git HEAD state — what build5 actually saw) into a
# policy-valid tree, and the assert catches the historical bug (with the
# old dot-glob the step silently did nothing).
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BSP_SRC="${1:-/home/noname/rnsbox/milkv-duo-01-arm}"
T=$(mktemp -d)
mkdir -p "$T/pristine/buildroot/configs" "$T/repo/board/common"
cp "$REPO/board/common/users.txt" "$T/repo/board/common/"
cp "$REPO/board/common/slim-target.sh" "$T/repo/board/common/"
# the three upstream (git HEAD) defconfigs = what a fresh clone/build saw
for f in cvitek_SG200X_32bit_defconfig cvitek_SG200X_64bit_defconfig cvitek_SG200X_musl_riscv64_defconfig; do
    git -C "$BSP_SRC" show "HEAD:buildroot/configs/$f" > "$T/pristine/buildroot/configs/$f" 2>/dev/null \
      || { echo "skip $f (not in HEAD)"; rm -f "$T/pristine/buildroot/configs/$f"; }
done
ls "$T/pristine/buildroot/configs/"
HERE="$T/repo"
sed -n "/^# ---- rootfs slimming/,/^# ---- board build steps/p" "$REPO/build.sh" | sed '$d' > "$T/fns.sh"
bash -n "$T/fns.sh" || { echo "FAIL: fns syntax"; exit 1; }
# shellcheck disable=SC1090
source "$T/fns.sh"

echo "--- slim the pristine tree"
rnsbox_slim_bsp "$T/pristine" || { echo "FAIL: slim errored on pristine"; exit 1; }

for dc in "$T/pristine"/buildroot/configs/cvitek_*_defconfig; do
    echo "--- $dc:"
    grep -E 'USERS_TABLES|ROOT_PASSWD|ENABLE_ROOT_LOGIN|DROPBEAR|OPENSSH_SERVER|PY_ONLY|PY_PYC|RESBLKS|POST_BUILD' "$dc"
done

echo "--- final assert (must pass):"
( rnsbox_assert_bsp_policy "$T/pristine" ) || { echo "FAIL: pristine tree not policy-valid after slim"; exit 1; }

echo "--- historical-bug regression: the old dot-glob must NOT match the real files"
if ls "$T/pristine"/buildroot/configs/cvitek_*.defconfig >/dev/null 2>&1; then
    echo "FAIL: dot-glob unexpectedly matches (test invalid)"
    exit 1
else
    echo "confirmed: cvitek_*.defconfig (dot) matches nothing — the build5 silent skip"
fi
rm -rf "$T"
echo "PRISTINE-SLIM-TEST-PASSED"
