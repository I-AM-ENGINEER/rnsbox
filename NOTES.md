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

## 9. RNode card: layout, transport labels, device list
Cause: the <section class="card"> opener was lost, so the RNode content
rendered glued to the Bluetooth card; the Transport options carried
board names (T-Echo, RAK4631) and device paths in the option text; the
USB device datalist was server-rendered once (no hot-plug, no way to
tell several modems apart without a reload).
Fix: restore the wrapper; options are Off / USB serial / BLE with the
details in the select title; the datalist refreshes from
/reticulum/rnode/status (devices[]) on every 15 s status poll.

## 5. rnsd watchdog banner "DOWN 3 min — reboots after 60 min" at State RUNNING
Cause: the banner trusted /run/rnsd-down-since alone. The writer
(deployed S46wanwatch, patches 0004+0024) removes it only when its own
liveness test passes: pidfile kill -0, else `pidof -x rnsd` — busybox
pidof has no -x, and a stale pidfile (dead pid, S82rnsd stop only waits
5 s for the python process) passes neither, while the portal finds the
live rnsd via a /proc cmdline scan. Contradiction, and the false streak
ends in a real reboot after down_limit.
Fix: rnsdmon_fill cross-checks sysinfo::rnsd_status().running first; if
rnsd is up the banner reads healthy and a stale numeric marker is
unlinked (best effort). The DOWN countdown still renders when rnsd is
really gone.

## 4. "System time … not synced" although the boot log says Stepping clock from NTP: OK
Cause: the portal reads /run/clock-source; the deployed duo-01 S45ntpsync
(patch 0004) only touches /run/clock-synced after a successful step and
never writes clock-source, so the indicator could never leave "not
synced" (until a browser sync).
Fix: sysinfo::clock_source() falls back to "ntp" when /run/clock-synced
exists and the wall clock is past the 2020 floor. The marker is removed
before each step attempt (never lies across boots), a browser sync still
writes clock-source=browser, and ntp keeps precedence for the browser
button + TLS clock gating (same call sites).

## 6. USB widget: "Running now: host" while the owner is on the gadget LAN
Cause: helpers/usbmode active_role() trusted /proc/cviusb/otg_role, which
reads "host" whenever the ID-pin override was not written this boot.
Fix: usb0 with carrier is ground truth — checked first on cvi and other
platforms, and required (plus a UDC) on rpi. RNSBOX_USB_NET_DIR test hook
added; smoke-tested 4 cases (host+carrier->device, device, host,
carrier0+bogus->unknown).

## 12. Hostname / echo bot / BLE name share one base prefix
Found: hostname default = "rnsbox" (vendor S10uuid applies /boot/hostname;
the build pins it there, patch 0003). Echo bot shipped default display name
"rnsbox echo bot" (patch 0027) — same prefix by luck, but hardcoded, so a
renamed box drifted. BLE advert defaulted to blank (UUID-only).
Fix (portal side; the hostname generation itself is the vendor script):
- echo bot: default name is now "<hostname> echo bot" (fallback "rnsbox
  echo bot"), applied at render + save.
- BLE: the Advertised-name field prefills with the hostname (trimmed to
  the 16-char BLE bound); clearing it still means UUID-only. Title
  documents the convention.

## 10. BLE transport: no pairing UX
Backend reality: the [[BLE Interface]] transport scans/accepts inside rnsd;
the only portal-facing BLE tooling was rnode.py scan/pair, filtered to
RNode adverts (NUS UUID + name prefix), and pairing is BlueZ JustWorks —
no user key exists anywhere in that flow.
Added (minimal honest UX on the Bluetooth card):
- rnode.py scan-any: every advertising device (name/address/bonded),
  no filter; GET /reticulum/bluetooth/scan serves it with a 15 s cache
  + single-flight lock (same pattern as the RNode scan).
- POST /reticulum/bluetooth/pair: alias over the existing generic
  bluetoothctl pair+trust (POST_ONLY updated). Scan table renders
  Name/Address/Bonded with a Pair button; paired rows say so.
- Pairing key field: rendered DISABLED with an honest "not implemented
  yet" note — RNS BLE takes no user key; a passkey/numeric-compare
  pairing would need an interactive agent wired into rnode.py pair.
  What is missing in the backend for full PIN UX: a bluetoothctl agent
  (PIN callback) in the pair helper + BlueZ agent registration.

## 11. No TCP Client Interface editor
Added a "TCP client interfaces" card to the Reticulum tab (before the raw
config editor): one editable row per [[...]] stanza with
type = TCPClientInterface (name/host/port), Save & apply + Delete per row,
an Add & apply form for new ones. Storage: the stanzas are edited IN PLACE
in /etc/reticulum/config (same model as the BLE stanza + HaLow quick-add —
the raw editor keeps showing the whole truth; no extra rnsd.conf sidecar
that could drift from the file rnsd actually parses). New routes
POST /reticulum/tcpclient/{add,update,delete} (POST_ONLY) restart rnsd on
success. Unit-tested on an x86 build against a fixture config: add dedupe
(name 2), default name, refusals (bad host chars, port range), in-place
update incl. rename-to-self, delete exactness ( neighbouring stanzas and
comments untouched, no file growth on repeated cycles).

## 1. Echo bot lived on its own tab
Now a card pair (status + settings) inside the Reticulum tab, right
after the rnsh card (anchor #echobot). pages/echobot.html keeps the two
card sections (renamed to "Echo bot"/"Echo bot settings", no page h2);
routes_echobot.cpp renders them through echobot_cards() which the
Reticulum page fills into __ECHOBOT_CARDS__. The old /echobot URL stays
a working bookmark: echobot_page() redirects to /reticulum#echobot, and
every PRG form already redirects there. Nav entry removed.

## 2. Updates tab is raw — hide from nav
The nav link is now `hidden` (kept in the markup with a
TODO(owners-decision) comment so the decision is greppable), and the
same TODO sits at the /updates dispatch in main.cpp. All /updates
routes and POST handlers stay fully functional by direct link, per the
owner instruction ("не удалять код... оставить по прямой ссылке").

## 3. "Remote access (rnsh)" tab merged into the RNS Shell card
The tab duplicated the server state (status/destination/announce) the
RNS Shell card already shows. access.html now carries only the unique
content — the issue/download form, the issued-grants table and the
manual-hashes table — reworded to point "above" instead of "on the
Reticulum tab"; the duplicated status table is dropped. routes_access
renders them through access_cards() into the Reticulum page directly
under the RNS Shell card (__RNSH_ACCESS_CARDS__, anchor #rnsh-access).
Nav entry removed; /access stays a working bookmark redirecting to
/reticulum#rnsh; all issue/revoke/reissue POSTs redirect there too.
