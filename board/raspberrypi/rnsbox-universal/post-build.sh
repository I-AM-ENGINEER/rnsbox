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
	# image-size: rcS/rcK too — the overlay ships them non-executable (the
	# git 644 mode overrides the skeleton's +x), and busybox init execve()s
	# rcS directly: without this the box hangs at "can't run rcS".
	chmod 0755 "${TARGET_DIR}"/etc/init.d/S* 	           "${TARGET_DIR}"/etc/init.d/rcS "${TARGET_DIR}"/etc/init.d/rcK 2>/dev/null || true
fi

# sec: secrets in the overlay ship group/world-readable (buildroot copies the
# overlay with the repo's 0644). The runtime services narrow these again
# (S35wifi chmods wifi.conf go-rwx; S82rnsd walks /etc/reticulum), but the
# image should already be right at first boot, before any S-script ran.
for f in etc/rnsbox/wifi.conf etc/rnsbox/rnsh.conf etc/reticulum/config; do
	if [ -f "${TARGET_DIR}/${f}" ]; then
		chmod 0600 "${TARGET_DIR}/${f}"
	fi
done
