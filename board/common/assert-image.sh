#!/bin/bash
# assert-image.sh — hard assertions on a built RNSBox rootfs TARGET dir.
#
#   ./assert-image.sh <target-dir> [cvitek|rpi|pc]
#
# Validates the buildroot OUTPUT (buildroot/output/target, or any unpacked
# rootfs tree — point it at the mounted/extracted root) against the shipped
# policy. Every check here maps to a real shipped bug (see the comment on
# each): the users-table/ssh knobs once silently never landed (build5), so
# images went out with no admin user and an always-on root sshd. Run it
# after every build — build.sh does not (yet) call it; CI/nightly wraps:
#
#   ./build.sh <board> lite && ./board/common/assert-image.sh \
#       <workdir>/buildroot/output/target cvitek
#
# Exit 0 = all assertions hold; exit 1 = list of failures printed.
set -u

T="${1:-}"
FLAVOR="${2:-cvitek}"
if [ -z "$T" ] || [ ! -d "$T" ]; then
    echo "usage: $0 <target-dir> [cvitek|rpi|pc]" >&2
    exit 1
fi
if [ ! -d "$T/usr" ]; then
    echo "assert-image: '$T' does not look like a rootfs TARGET dir (no usr/)" >&2
    exit 1
fi

fails=0
ok()   { printf '  [ok]   %s\n' "$1"; }
bad()  { printf '  [FAIL] %s\n' "$1"; fails=$((fails + 1)); }
warn() { printf '  [warn] %s\n' "$1"; }

echo "== assert-image: $T (flavor: $FLAVOR)"

# ---- users ----------------------------------------------------------------
# NB: the users TABLE (admin) is applied by buildroot's fakeroot step at
# IMAGE creation (fs/common.mk), NOT in output/target — so the passwd and
# shadow checks read the built rootfs.ext2/4 (debugfs) when it sits next
# to the target dir, and fall back to the plain files otherwise.
IMG=""
for cand in "$T/../images/rootfs.ext2" "$T/../images/rootfs.ext4" \
            "$T/images/rootfs.ext2" "$T/images/rootfs.ext4"; do
    [ -e "$cand" ] && IMG="$cand" && break
done
get_file() { # $1 = path inside the image/target
    if [ -n "$IMG" ] && command -v debugfs >/dev/null 2>&1; then
        debugfs -R "cat $1" "$IMG" 2>/dev/null
    else
        [ -n "$IMG" ] && warn "debugfs not found — passwd/shadow checks read the staging tree" 
        cat "$T$1" 2>/dev/null
    fi
}
passwd_file=$(get_file /etc/passwd)
shadow_file=$(get_file /etc/shadow)
# The portal sets the admin password with `passwd -a sha512 admin` (Settings
# -> SSH); without the user the SSH enable path fails with "passwd failed".
if grep -qE '^admin:' <<<"$passwd_file"; then
    ok "admin user present in /etc/passwd"
    home=$(grep -E '^admin:' <<<"$passwd_file" | cut -d: -f6)
    shell=$(grep -E '^admin:' <<<"$passwd_file" | cut -d: -f7)
    [ "$shell" != "/bin/false" ] || bad "admin shell is /bin/false"
    [ -n "$home" ] || bad "admin has no home dir field"
else
    bad "admin user MISSING from /etc/passwd (users table did not land)"
fi
# admin must be password-LOCKED at build time ('!' in shadow field 2) — the
# password is set on-device, never baked.
shadow_admin=$(grep -E '^admin:' <<<"$shadow_file" | cut -d: -f2)
case "$shadow_admin" in
    '!'|'!!') ok "admin password locked in /etc/shadow" ;;
    '')       bad "admin missing from /etc/shadow" ;;
    '*')      warn "admin password '*' (locked, unusual — prefer '!')" ;;
    *)        bad "admin shadow field not locked (leaked build-time password?)" ;;
esac
# root must carry NO usable password: buildroot locks it '*' when
# BR2_TARGET_ENABLE_ROOT_LOGIN is off.
shadow_root=$(grep -E '^root:' <<<"$shadow_file" | cut -d: -f2)
case "$shadow_root" in
    '*'|'!'|'!!') ok "root password locked in /etc/shadow" ;;
    '')           warn "root missing from shadow" ;;
    *)            bad "root password hash present in shipped shadow (root login must stay closed)" ;;
esac

# ---- ssh daemon -----------------------------------------------------------
if [ "$FLAVOR" = cvitek ]; then
    # dropbear must never ship as a second daemon next to openssh
    if [ -e "$T/usr/sbin/dropbear" ] || [ -e "$T/etc/init.d/S50dropbear" ]; then
        bad "dropbear present in the image (openssh is THE ssh daemon)"
    else
        ok "no dropbear in the image"
    fi
    if [ -e "$T/usr/sbin/sshd" ]; then
        ok "openssh /usr/sbin/sshd present"
    else
        bad "openssh server missing (BR2_PACKAGE_OPENSSH_SERVER did not land)"
    fi
    # the S50sshd gate: sshd runs ONLY while /etc/rnsbox/ssh.conf says
    # enabled=yes. The boot-on-disabled-sshd bug shipped before this gate.
    if grep -q 'ssh.conf' "$T/etc/init.d/S50sshd" 2>/dev/null \
       && grep -q 'ssh_enabled' "$T/etc/init.d/S50sshd" 2>/dev/null; then
        ok "S50sshd is the gated variant (enabled=no -> never starts)"
    else
        bad "S50sshd is NOT the gated variant (stock buildroot script starts sshd unconditionally)"
    fi
    if [ -f "$T/etc/rnsbox/ssh.conf" ]; then
        if grep -qE '^enabled=no' "$T/etc/rnsbox/ssh.conf"; then
            ok "ssh.conf ships enabled=no"
        else
            bad "ssh.conf does not ship enabled=no"
        fi
    else
        bad "etc/rnsbox/ssh.conf missing (gate has nothing to read)"
    fi
    if grep -qE '^PermitRootLogin no' "$T/etc/ssh/sshd_config" 2>/dev/null; then
        ok "sshd_config: PermitRootLogin no"
    else
        bad "sshd_config does not close root login"
    fi
    # lazy host keys: no key material baked into the image; ssh-keygen -A
    # runs on the FIRST enabled start (S50sshd gate) or first manual start.
    ls "$T"/etc/ssh/ssh_host_* >/dev/null 2>&1 \
        && warn "host keys baked into the image (policy: generate lazily on first enable)" \
        || ok "no baked ssh host keys (generated lazily)"
fi

# ---- services the boards rely on -----------------------------------------
[ -e "$T/etc/init.d/S82rnsd" ] && ok "S82rnsd present" || bad "S82rnsd missing"
[ -e "$T/etc/init.d/S60routing" ] && ok "S60routing present" || bad "S60routing missing"
[ -e "$T/usr/sbin/dnsmasq" ] && ok "dnsmasq present" || warn "dnsmasq not installed"
[ -e "$T/etc/init.d/S80dnsmasq" ] || warn "no S80dnsmasq init script"

if [ "$FLAVOR" = cvitek ]; then
    [ -e "$T/etc/init.d/S35wifi" ] && ok "S35wifi (AIC8800 bring-up) present" || bad "S35wifi missing"
    # the radio kos are board-SKU content: present on -W images, soft on the
    # rest (S35wifi exits 0 when no wlan appears — "no WiFi" must stay soft)
    if ls "$T"/mnt/system/ko/3rd/aic8800_bsp.ko >/dev/null 2>&1; then
        ok "aic8800_bsp.ko shipped"
    else
        warn "aic8800_bsp.ko absent (non-W image — acceptable, error stays soft)"
    fi
    [ -e "$T/etc/init.d/S31slip" ] && ok "S31slip present" || bad "S31slip missing"
fi

# ---- python shape (py-only policy) ----------------------------------------
pydir=$(ls -d "$T"/usr/lib/python3.* 2>/dev/null | head -1)
if [ -n "$pydir" ]; then
    if find "$pydir" -name '*.pyc' 2>/dev/null | grep -q .; then
        bad "py-only policy: .pyc files shipped (BR2_PACKAGE_PYTHON3_PY_ONLY did not land)"
    else
        ok "py-only: no .pyc payload in the rootfs"
    fi
else
    warn "no python3 stdlib dir found (non-python image?)"
fi

# ---- slimmer effect (post-build hook ran) ---------------------------------
if [ -d "$pydir" ] && [ -d "$pydir/ensurepip" ]; then
    bad "slim-target hook did not run (ensurepip still present)"
else
    ok "slim-target hook effect present (ensurepip stripped)"
fi

echo "== assert-image: $fails failure(s)"
[ "$fails" = 0 ]
