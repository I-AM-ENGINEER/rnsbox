#!/bin/sh
# RNSBox post-build: the stock buildroot raspberrypi post-build inlined (in
# the external tree there is no ../post-build.sh to delegate to — it lives
# in the buildroot checkout), then keep every overlay init script executable
# (the patch tooling does not preserve the +x bit; both the busybox rc and
# the portal's subprocess execve() reject non-executable S* scripts).
set -e
mkdir -p "${TARGET_DIR}/boot" "${TARGET_DIR}/var/lock"

# Add a console on tty1 (from buildroot/board/raspberrypi/post-build.sh)
if [ -e "${TARGET_DIR}/etc/inittab" ]; then
	grep -qE '^tty1::' "${TARGET_DIR}/etc/inittab" || \
		sed -i '/GENERIC_SERIAL/a\
tty1::respawn:/sbin/getty -L  tty1 0 vt100 # HDMI console' "${TARGET_DIR}/etc/inittab"
fi

if [ -d "${TARGET_DIR}/etc/init.d" ]; then
	chmod 0755 "${TARGET_DIR}"/etc/init.d/S* 2>/dev/null || true
fi
