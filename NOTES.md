# portal-ui-fixes — working notes (bug -> cause -> fix -> verification)

Branch: portal-ui-fixes (clone of /mnt/c/.../GitHub/rnsbox main@5e9c52c).
One bug = one commit. Live board checks deferred until the owner reflashes
(SSH admin/rnsbox-test1 on 192.168.88.230); all logic verified by a local
x86-64 build of the CGI + stdin request runs (see the last section).

## Deployed-image context discovered while digging (duo-01 patches)
The Duo Module 01 image is built from patches/milkv-duo-01/*.patch, which ship
their OWN S45ntpsync / S46wanwatch / S82rnsd into
buildroot/board/cvitek/SG200X/overlay (patches 0004, 0008, 0024). Those differ
from the richer board/milkv-duo/rootfs-overlay versions:

- S45ntpsync (0004) writes ONLY /run/clock-synced (touch); it NEVER writes
  /run/clock-source. Its boot log line: "Stepping clock from NTP: OK".
  The portal's clock indicator reads /run/clock-source -> always "not synced".
- S46wanwatch (0004+0024) writes /run/rnsd-down-since only when its own
  liveness test fails: pidfile kill -0, else `pidof -x rnsd`. busybox pidof
  has no -x, and a stale pidfile (dead pid, not yet recycled) passes nothing,
  so the file can persist while the real rnsd (found by the portal's /proc
  cmdline scan) is alive -> contradictory banner.
- S82rnsd (0008) writes /var/run/rnsd.pid itself; stop() waits only 5 s for
  the python process to exit, so a stop->start race can leave the pidfile
  naming a dead pid while the older rnsd still holds the port.
- Hostname: the vendor S10uuid reads /boot/hostname (patch 0003 pins it to
  "rnsbox"), else generates <board>-<keyhash>. The echobot ships the default
  display name "rnsbox echo bot" (patch 0027 echobot.conf) — same "rnsbox"
  base prefix already.
