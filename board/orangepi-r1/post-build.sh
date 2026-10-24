#!/bin/sh
# RNSBox post-build for the Orange Pi R1: keep every overlay init script
# executable (the git checkout does not preserve the +x bit; busybox rc and
# the portal's execve() reject non-executable S* scripts) and narrow the
# permissions of the shipped secrets.
set -e

mkdir -p "${TARGET_DIR}/boot" "${TARGET_DIR}/var/lock"

if [ -d "${TARGET_DIR}/etc/init.d" ]; then
	chmod 0755 "${TARGET_DIR}"/etc/init.d/S* \
	           "${TARGET_DIR}"/etc/init.d/rcS "${TARGET_DIR}"/etc/init.d/rcK 2>/dev/null || true
fi

for f in etc/rnsbox/wifi.conf etc/rnsbox/rnsh.conf etc/reticulum/config; do
	if [ -f "${TARGET_DIR}/${f}" ]; then
		chmod 0600 "${TARGET_DIR}/${f}"
	fi
done
