#!/bin/bash
# Functional test for the build.sh slim/policy functions (rnsbox_slim_bsp,
# rnsbox_assert_bsp_policy, rnsbox_stamp_live_config) against a fake BSP tree.
set -u
T=$(mktemp -d)
mkdir -p "$T/bsp/buildroot/configs" "$T/repo/board/common"
cp "$(dirname "$0")/../board/common/users.txt" "$T/repo/board/common/"
cp "$(dirname "$0")/../board/common/slim-target.sh" "$T/repo/board/common/"
fail=0
cat > "$T/bsp/buildroot/configs/cvitek_SG200X_64bit_defconfig" <<'EOF'
BR2_TARGET_ENABLE_ROOT_LOGIN=y
BR2_TARGET_GENERIC_ROOT_PASSWD="admin"
BR2_ROOTFS_USERS_TABLES=""
BR2_ROOTFS_POST_BUILD_SCRIPT=""
# BR2_PACKAGE_PYTHON3_PY_ONLY is not set
BR2_PACKAGE_PYTHON3_PY_PYC=y
BR2_TARGET_ROOTFS_EXT2_RESBLKS=5
# BR2_PACKAGE_OPENSSH_SERVER is not set
EOF
HERE="$T/repo"
sed -n "/^# ---- rootfs slimming/,/^# ---- board build steps/p" \
    "$(dirname "$0")/../build.sh" | sed '$d' > "$T/fns.sh"
bash -n "$T/fns.sh" || { echo "FAIL: extracted fns syntax"; exit 1; }
# shellcheck disable=SC1090
source "$T/fns.sh"

echo "--- run 1: upstream-shaped defconfig (knobs present but wrong values)"
rnsbox_slim_bsp "$T/bsp" || { echo "FAIL: slim run 1 errored"; fail=1; }
dc="$T/bsp/buildroot/configs/cvitek_SG200X_64bit_defconfig"
for want in \
    'BR2_PACKAGE_PYTHON3_PY_ONLY=y' \
    'BR2_TARGET_ROOTFS_EXT2_RESBLKS=1' \
    'BR2_ROOTFS_POST_BUILD_SCRIPT="board/common/slim-target.sh"' \
    'BR2_TARGET_GENERIC_ROOT_PASSWD="*"' \
    'BR2_ROOTFS_USERS_TABLES="board/common/users.txt"' \
    'BR2_PACKAGE_OPENSSH_SERVER=y'
do
    grep -qxF "$want" "$dc" || { echo "FAIL: missing knob after slim: $want"; fail=1; }
done
grep -q '^BR2_TARGET_ENABLE_ROOT_LOGIN=y' "$dc" && { echo "FAIL: root login still enabled"; fail=1; }
grep -q '^BR2_PACKAGE_PYTHON3_PY_PYC=y' "$dc" && { echo "FAIL: pyc still on"; fail=1; }
[ -f "$T/bsp/buildroot/board/common/users.txt" ] || { echo "FAIL: users.txt not copied"; fail=1; }
[ -x "$T/bsp/buildroot/board/common/slim-target.sh" ] || { echo "FAIL: slimmer not copied/executable"; fail=1; }

echo "--- run 2: idempotency"
rnsbox_slim_bsp "$T/bsp" >/dev/null 2>&1 || { echo "FAIL: slim run 2 errored"; fail=1; }

echo "--- live config stamp (stale values in buildroot/.config)"
cp "$dc" "$T/bsp/buildroot/.config"
sed -i 's/BR2_TARGET_GENERIC_ROOT_PASSWD=.*/BR2_TARGET_GENERIC_ROOT_PASSWD="admin"/' "$T/bsp/buildroot/.config"
rnsbox_stamp_live_config "$T/bsp/buildroot/.config"
grep -q '^BR2_TARGET_GENERIC_ROOT_PASSWD="\*"$' "$T/bsp/buildroot/.config" \
    || { echo "FAIL: live config root passwd not stamped"; fail=1; }
grep -q '^BR2_PACKAGE_PYTHON3_PY_ONLY=y$' "$T/bsp/buildroot/.config" \
    || { echo "FAIL: live config PY_ONLY missing"; fail=1; }

echo "--- negative: a present-but-wrong knob must fail loudly"
sed -i 's/BR2_ROOTFS_USERS_TABLES=.*/BR2_ROOTFS_USERS_TABLES=""/' "$dc"
if ( rnsbox_assert_bsp_policy "$T/bsp" ) 2>"$T/err"; then
    echo "FAIL: assert passed with empty users table"; fail=1
else
    grep -q "policy knob did not land" "$T/err" || { echo "FAIL: wrong error text"; fail=1; }
    head -1 "$T/err"
fi

echo "--- negative: no cvitek defconfigs must fail loudly"
mkdir -p "$T/empty/buildroot/configs"; touch "$T/empty/buildroot/configs/other_defconfig"
if ( rnsbox_slim_bsp "$T/empty" ) 2>"$T/err2"; then
    echo "FAIL: empty glob not caught"; fail=1
else
    grep -q "no cvitek" "$T/err2" || { echo "FAIL: wrong empty-glob error"; fail=1; }
    head -1 "$T/err2"
fi

echo "--- negative: not a BSP tree must fail loudly"
if ( rnsbox_slim_bsp "$T/nonexistent" ) 2>"$T/err3"; then
    echo "FAIL: non-BSP not caught"; fail=1
else
    grep -q "not a BSP tree" "$T/err3" || { echo "FAIL: wrong non-BSP error"; fail=1; }
fi

echo "--- self-heal: a defconfig missing a knob entirely gets it appended"
rm -rf "$T/bsp2"; mkdir -p "$T/bsp2/buildroot/configs"
cat > "$T/bsp2/buildroot/configs/cvitek_test_defconfig" <<'EOF'
BR2_TARGET_OPENSSH_SERVER=y
EOF
sed -i 's/^BR2_TARGET_OPENSSH_SERVER=y/BR2_PACKAGE_OPENSSH_SERVER=y/' "$T/bsp2/buildroot/configs/cvitek_test_defconfig"
if rnsbox_slim_bsp "$T/bsp2" >/dev/null 2>&1; then
    grep -qxF 'BR2_ROOTFS_USERS_TABLES="board/common/users.txt"' "$T/bsp2/buildroot/configs/cvitek_test_defconfig" \
        || { echo "FAIL: missing knob not appended"; fail=1; }
    echo "appended-ok"
else
    echo "FAIL: self-heal run errored"; fail=1
fi

rm -rf "$T"
if [ "$fail" = 0 ]; then echo "ALL-SLIM-TESTS-PASSED"; else echo "SLIM-TESTS-FAILED"; exit 1; fi
