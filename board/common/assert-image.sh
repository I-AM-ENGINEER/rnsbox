#!/bin/bash
# assert-image.sh — hard assertions on a built RNSBox rootfs TARGET dir.
#
#   ./assert-image.sh <target-dir> [cvitek|rpi|pc] [kernel.dtb|images-dir]
#
# The optional third argument is the kernel DTB (or a directory to search
# for one) for the DRAM-map assertions: the DTB memory node must be
# >= 440 MB on a 512 MB board and the ION reserved-memory carveout must be
# <= 32 MB (the memmap shrink regression guard).
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
DTB="${3:-}"
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
        # The bsp compiles CONFIG_SDIO_BT=y (vendor stock): it boots the
        # combined WiFi+BT firmware and loads the BT patch files, so the
        # fmacfwbt set MUST be on every -W image even when the HCI transport
        # is absent (rv64 kernels keep CONFIG_BT off).
        FWD="$T/usr/lib/firmware/aic8800_sdio/aic8800_and_aic8800D80"
        for fw in fmacfwbt.bin fw_adid.bin fw_patch.bin fw_patch_table.bin; do
            [ -e "$FWD/$fw" ] && ok "aic8800 BT firmware $fw shipped"                 || bad "aic8800 BT firmware $fw missing (bsp boots fmacfwbt)"
        done
        # The -W SDIO radio is an AIC8800D80 (proven live: vid 0xc8a1 did
        # 0x0082): it boots the *_8800d80_u02 set — a past slim rule deleted
        # these as "USB-dongle silicon" and the radio could not boot WiFi.
        for fw in fmacfwbt_8800d80_u02.bin fw_adid_8800d80_u02.bin                   fw_patch_8800d80_u02.bin fw_patch_table_8800d80_u02.bin                   fw_patch_8800d80_u02_ext0.bin; do
            [ -e "$FWD/$fw" ] && ok "aic8800D80 firmware $fw shipped"                 || bad "aic8800D80 firmware $fw missing (radio boots this set)"
        done
        # The BlueZ transport is arm64-only for now: the rv64/cv1800b kernels
        # ship CONFIG_BT off, btsdio.o would not link there.
        if [ -e "$T/lib/ld-linux-aarch64.so.1" ]; then
            if strings "$T"/mnt/system/ko/3rd/aic8800_fdrv.ko 2>/dev/null | grep -q 'btsdio_init'; then
                ok "aic8800_fdrv.ko carries the btsdio (BlueZ HCI) transport"
            else
                bad "aic8800_fdrv.ko has no btsdio transport (BT stays dead on -W arm64)"
            fi
            [ -e "$T"/mnt/system/ko/3rd/aic8800_btlpm.ko ]                 && ok "aic8800_btlpm.ko shipped (optional LPM/rfkill, not auto-loaded)"                 || bad "aic8800_btlpm.ko missing"
        fi
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

# ---- python unicodedata / idna codec (the silent-rnsd-killer guard) -------
# The 64bit cvitek defconfigs once shipped with BR2_PACKAGE_PYTHON3_UNICODEDATA
# unset: buildroot configured CPython --disable-unicodedata, the target had
# no lib-dynload/unicodedata C extension, `import encodings.idna` failed at
# codec-load time and EVERY socket.getaddrinfo() raised
# LookupError: unknown encoding: idna. rnsd creates its TCPServerInterface
# (listen_ip = 0.0.0.0) right after start -> unhandled exception -> RNS
# os._exit -> empty /var/log/rnsd.log -> wanwatch reboot loop. Shipping an
# image without this extension ships a dead rnsd.
if [ -n "$pydir" ]; then
    if ls "$pydir"/lib-dynload/unicodedata*.so >/dev/null 2>&1; then
        ok "python unicodedata extension present (idna codec loadable)"
    else
        bad "python unicodedata MISSING (encodings.idna unimportable - rnsd dies on its TCPServerInterface)"
    fi
    if [ -f "$pydir/encodings/idna.py" ]; then
        ok "encodings/idna.py present"
    else
        bad "encodings/idna.py missing (getaddrinfo cannot resolve anything)"
    fi
fi

# ---- /tmp tmpfs shape (the stacked-mount bug) ------------------------------
# The buildroot skeleton symlinks /var/{log,cache,spool,tmp} -> ../tmp, so
# the old per-dir /var/* tmpfs fstab entries stacked THREE anonymous mounts
# onto /tmp and the last one (512k, mode 0755) won: admin could not write
# /tmp at all and /var/log lived on that same tiny layer.
if [ -f "$T/etc/fstab" ]; then
    if grep -qE '^tmpfs[[:space:]]+/tmp[[:space:]]+tmpfs[[:space:]]+.*mode=1777' "$T/etc/fstab" \
       && ! grep -qE '^tmpfs[[:space:]]+/var/(log|cache|lock|spool|tmp)[[:space:]]' "$T/etc/fstab"; then
        ok "fstab: single 1777 tmpfs on /tmp, no stacked /var/* tmpfs mounts"
    else
        bad "fstab: /tmp tmpfs shape wrong (want one mode=1777 /tmp entry and NO /var/* tmpfs lines - they stack onto /tmp through the skeleton symlinks)"
    fi
fi

# ---- slimmer effect (post-build hook ran) ---------------------------------
if [ -d "$pydir" ] && [ -d "$pydir/ensurepip" ]; then
    bad "slim-target hook did not run (ensurepip still present)"
else
    ok "slim-target hook effect present (ensurepip stripped)"
fi

# ---- DRAM map (memmap regression guard) -----------------------------------
# A 512 MB board must boot with a >= 440 MB kernel memory node, and the ION
# reserved-memory carveout inside it must stay small (the media pipeline is
# unused on a headless router). Both facts live in the kernel DTB the boot
# chain actually ships - the memory node from CVIMMAP_KERNEL_MEMORY_*, the
# ion-region from CVIMMAP_ION_SIZE - so the decompiled DTB is the ground
# truth, not the rootfs.
check_dtb() {
    local dtb="$1" dtc_txt mem_tok ion_tok mem_mb ion_mb
    command -v dtc >/dev/null 2>&1 || { warn "dtc not on PATH - DTB RAM assertions skipped"; return 0; }
    if [ -d "$dtb" ]; then
        dtb="$(find "$dtb" -name '*.dtb' | head -1)"
        [ -n "$dtb" ] || { warn "no .dtb under the given dir - DTB RAM assertions skipped"; return 0; }
    fi
    dtc_txt="$(dtc -I dtb -O dts -q "$dtb" 2>/dev/null)" || { bad "kernel DTB '$dtb' does not decompile"; return 0; }
    mem_tok="$(awk '/device_type = "memory";/{f=1} f && /reg = </{print; exit}' <<<"$dtc_txt" | grep -oE '0x[0-9a-fA-F]+' | tail -1)"
    ion_tok="$(awk '/ion-region/{f=1} f && /size = </{print; exit}' <<<"$dtc_txt" | grep -oE '0x[0-9a-fA-F]+' | tail -1)"
    if [ -n "$mem_tok" ]; then
        mem_mb=$(( mem_tok / 1024 / 1024 ))
        if [ "$mem_mb" -ge 440 ]; then
            ok "DTB memory node: ${mem_mb} MB (>= 440 MB)"
        else
            bad "DTB memory node: only ${mem_mb} MB (expected >= 440 MB on a 512 MB board - memmap regression?)"
        fi
    else
        bad "DTB has no parseable memory node reg"
    fi
    if [ -n "$ion_tok" ]; then
        ion_mb=$(( ion_tok / 1024 / 1024 ))
        if [ "$ion_mb" -le 32 ]; then
            ok "DTB ion reserved-memory: ${ion_mb} MB (<= 32 MB, media unused)"
        else
            bad "DTB ion reserved-memory: ${ion_mb} MB (> 32 MB - the 170M vendor carveout is back)"
        fi
    else
        warn "DTB has no ion-region (ion absent - fine if intended)"
    fi
}
if [ -n "$DTB" ]; then
    check_dtb "$DTB"
else
    warn "no DTB argument - DRAM-map assertions skipped (pass the kernel .dtb or an images dir)"
fi

echo "== assert-image: $fails failure(s)"
[ "$fails" = 0 ]
