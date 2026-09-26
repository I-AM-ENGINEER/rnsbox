#!/bin/sh
# RNSBox post-build: run the stock Raspberry Pi post-build (tty1 console),
# then keep every overlay init script executable (the patch tooling does not
# preserve the +x bit; both the busybox rc and the portal's subprocess
# execve() reject non-executable S* scripts).
set -e
mkdir -p "${TARGET_DIR}/boot" "${TARGET_DIR}/var/lock"
exec_sh="$(dirname "$0")/../post-build.sh"
"$exec_sh" "$@"
if [ -d "${TARGET_DIR}/etc/init.d" ]; then
	chmod 0755 "${TARGET_DIR}"/etc/init.d/S* 2>/dev/null || true
fi
