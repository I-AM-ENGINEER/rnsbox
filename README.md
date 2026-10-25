# RNSBox — a Reticulum router

RNSBox turns a small, USB-powered single-board computer into a
[Reticulum](https://reticulum.network/) router and transport node with an
OpenWrt-style web admin UI. One repository builds every supported board —
from a 64 MB Milk-V Duo to a Raspberry Pi 5 or a generic PC — with the
router software shared and only the board files differing.

The repository is a Buildroot **external tree**, and there is exactly one
build flow: a pinned upstream Buildroot + mainline Linux + this repo as
`BR2_EXTERNAL`. The only vendor artifact left is the CVITEK first-stage
boot blob (`fip.bin`), which a board's post-image script writes to the boot
FAT; there is no vendor Buildroot, no per-board packer and no patch series.

## Supported boards

| `./build.sh` key | Board | Core | Kernel |
|---|---|---|---|
| `rpi-universal` | Raspberry Pi Zero 2 W / 3B / 3B+ / 3A+ / CM3 / 4B / 400 / CM4 / 5 / 500 / CM5 — **one image for all** | A53/BCM2711-class (aarch64) | Raspberry Pi 6.12 |
| `sg2002-licheerv-nano-rv64` | Sipeed LicheeRV Nano(-e / -e **W**) | RISC-V C906 | mainline **7.3** |
| `milkv-duo-cv1800b-rv64` | Milk-V Duo 64M | RISC-V C906 | mainline **7.3** |
| `sg2002-duo256m-rv64` | Milk-V Duo 256M | RISC-V C906 | mainline **7.3** |
| `sg2000-duomodule01-rv64` | Milk-V Duo Module 01 (EVB) | RISC-V C906 | mainline **7.3** |
| `sg2000-duo-s-rv64` | Milk-V Duo S | RISC-V C906 | mainline **7.3** |
| `sg2000-duo-s-arm64` | Milk-V Duo S | ARM Cortex-A53 | mainline **7.3** |
| `orangepi-r1` | Orange Pi R1 (Allwinner H2+) | ARM Cortex-A7 | mainline 6.6 |
| `pc-x86_64` | any PC / QEMU / VirtualBox disk | x86-64 | 6.12 |

The CVITEK family (SG2000 / SG2002 / CV1800B) runs a genuine kernel.org
**mainline Linux 7.3** with only DTS/driver patches carried in-tree — the
vendor 5.10 BSP is gone entirely. Board files also exist for the
**Heltec HT-HD01-V2** (MT7628AN + Morse HaLow radio, `ht-hd01-v2` core/full
defconfigs); it is not yet wired into `build.sh` and the CI matrix.

Common base: [Buildroot](https://buildroot.org) 2026.02.3 LTS.

## Quick start

Prebuilt images for every board are published on the
[GitHub Releases](https://github.com/I-AM-ENGINEER/rnsbox/releases) page
(built by CI on every push to `main`, with SHA-256 checksums in the notes).

To build yourself:

```bash
git clone https://github.com/I-AM-ENGINEER/rnsbox
cd rnsbox
./build.sh rpi-universal               # Raspberry Pi Zero 2 W / 3 / 4 / 5
./build.sh sg2002-licheerv-nano-rv64   # LicheeRV Nano-e (C906)
./build.sh all                         # every board, sequentially
```

The finished image lands at
`~/rnsbox-work/upstream/out/<board>/images/rnsbox-<board>.img`. Flash it with
`dd` (or a tool like balenaEtcher) to a microSD and boot the board; the
rootfs grows to fill the card on first boot.

> **First build is long.** Buildroot compiles the kernel, OpenSSL, Python and
> the rest of the distro from scratch. Later builds are incremental.

Build host: a Linux machine set up for Buildroot (WSL2 works too).
`./build.sh` needs `/usr/sbin` on `PATH` for genimage's `mkdosfs`; it
handles this itself.

## First boot

- Plug the board's USB port into your computer: it enumerates as a
  CDC-NCM network card. Open **http://10.42.0.1/** and log in
  (`admin` / `admin`), or join the WiFi hotspot the box raises by default
  (SSID `rnsbox-xxxxxx`, password `reticulum`).
- Change the admin password on first use — Settings → *Change admin
  password* updates the web and the Unix/SSH account at once.
- Root login is closed everywhere; `admin` is the operator account and has
  passwordless sudo.

## What it does

### Networking

- **`rnsd`** (Reticulum, always the latest release at build time) runs as a
  long-lived service from every boot. Transport node with a
  `TCPServerInterface` on `0.0.0.0:4242`, an `AutoInterface` on the LAN and
  two public RNS-testnet uplinks preconfigured; interface discovery is on.
- **USB gadget = LAN.** The board presents a CDC-NCM network interface, and
  the WiFi hotspot joins it as a second port of one bridged LAN
  (`br-lan = 10.42.0.1/24`, dnsmasq DHCP + DNS).
- **WAN**, DHCP or static, with automatic uplink selection (wired where
  present, else the WiFi station), NAT masquerade plus per-rule port
  forwarding and open-port management (tcp/4242 stays pinned open). The
  portal is the single source of truth for the generated `nftables`
  ruleset; the firewall comes up fail-closed and IPv6 is confined to the
  LAN side.
- **WiFi** where the board has a radio (AIC8800 on the SG200x *W* variants,
  brcmfmac on the Pi, RTL8189ETV on the Orange Pi R1): STA and AP run
  independently (hotspot + uplink concurrently), WPA2 / WPA3 / transition,
  2.4 and 5 GHz with band/width/channel/country control, hidden networks,
  continuous background scan, and a live list of connected hotspot clients.
  The radio MAC is stable across reboots.

### Web portal

**`rnsbox-portal`** — a compact C++/CGI admin UI served by uhttpd on
`http://10.42.0.1/`:

- **Dashboard** with 1 s live refresh: CPU (5 s window), RAM/swap,
  interfaces, rnsd status, and per-transport cards (RNode radio telemetry,
  HaLow link, BLE peers with RX/TX/RSSI/link time).
- **Reticulum tab**: a live rnstatus table (per-interface status, rates,
  peers, RX/TX), the HaLow modem card, the BLE transport card, the RNode
  card and the RNS Shell / Echo bot cards.
- **Map tab**: nodes that announce coordinates are drawn on a bundled
  offline world map (Natural Earth); discovered nodes accumulate on flash
  and survive reboots. Nothing is fetched from the internet.
- **Manual tab**: the official Reticulum manual, bundled as Markdown and
  rendered by the browser — instant on slow boards, refreshable from
  upstream on demand.
- **Console and NomadNet tabs**: a real shell (zsh) and the full NomadNet
  TUI in the browser, over a WebSocket transport with server-side screen
  diffs — fast even on the small boards and slow links. Either opens
  full-window in its own browser tab.
- **Settings**: one password for the web and the Unix/SSH account, SSH
  enable/disable, USB port role (device/host), a UART card (per-port
  console/SLIP/RNode/custom assignment with the board's pin table), clock
  (sync from the browser, NTP servers, Reticulum time below), theme.
- **Diagnostics**: on boot the box writes a health report to
  `/boot/rnsbox-diag.txt` on the boot FAT (readable from any PC without a
  console), and an empty `/boot/reset-admin` file on the card resets the
  admin password back to `admin`/`admin`.
- A watchdog card can reboot the box when rnsd has been down for a
  configurable period.

### Transports and radios

- **RNode (USB)**: the RNode card drives `rnodeconf` for attached modems —
  device info, configuration, live counters and the display framebuffer
  (PNG), plus a LoRa air-rate calculator (bitrate/sensitivity/SNR from
  BW/SF/CR) and discovery settings (coordinates, announce interval).
- **BLE transport**: the box advertises a Reticulum GATT service and can
  also connect to peers that only advertise (Columba, Sideband). The same
  adapter exposes an **RNode-over-BLE** endpoint (Nordic UART Service):
  pair the phone, pick the box as an RNode, and its packets are bridged
  into the local transport — useful for a portable node.
- **HaLow modem**: an external WiFi-HaLow (RNode) modem over a 3-wire SLIP
  UART link (2 Mbaud on SG200x/Pi, 1.5 Mbaud on the CV1800B Duo) or over
  TCP, with the modem's own web UI reverse-proxied through the portal
  login. Enabling the link adds the modem's transport interface to the
  rnsd config automatically.
- **HaLow flasher tab**: discovers factory (HGIC) modems on the link,
  flashes firmware — from a GitHub release, a local file or the image's
  bundled copy — over raw-Ethernet or HTTP OTA, shows live progress and a
  detailed log, and can auto-update each modem on a staggered schedule.
- **RNS Shell (`rnsh`)**: a remote shell over Reticulum, enabled by default
  but strictly allow-listed — keys are issued and revoked from the portal
  as downloadable identity files, and the service refuses to run with an
  empty list.
- **Echo bot**: a local LXMF echo-bot service that replies to any message,
  with per-peer traffic statistics in the portal.
- Optional **NomadNet** LXMF/pages node and **`lxmd`** LXMF propagation
  node (both off by default).

### Time

- The clock syncs in the background once a WAN exists (NTP), can be seeded
  from your browser, and an optional DS3231 RTC keeps time powered-off
  where wired (Pi: SDA GPIO2/pin 3, SCL GPIO3/pin 5).
- **Time over Reticulum** (`rns-time`): when ordinary NTP is unreachable
  (LoRa, HaLow, LAN-only deployments), the clock steps from an
  authenticated time server on the mesh; NTP always wins. A box with an
  uplink can serve time to the rest of the mesh. See
  [Time over Reticulum](#time-over-reticulum).

### Under the hood

- A/B whole-image OS updater with crash-safe transactions and rollback
  (experimental; not yet exposed in the menu).
- zram swap is enabled automatically on boards with under 256 MB of RAM,
  and the stock board middleware (camera / display / audio / NPU / codec)
  is stripped so nearly all of the RAM is available to Linux and the
  router data plane.
- Each board gets a stable unique hostname (`rnsbox-xxxxxx`) used for the
  hotspot SSID and the BLE advert.

## USB modems and RNodes

The Raspberry Pi images carry the USB serial and modem drivers (`option`,
`qmi_wwan`, `cdc_acm`, `cdc_ncm`/`ecm`, `ch341`, `cp210x`, `ftdi_sio`). On a
Pi 3/4/5 plug the modem or RNode into a USB-A port. The Zero 2 W has a
single micro-USB port that the image uses as the CDC-NCM LAN gadget: to use
a USB modem there instead, set `dtoverlay=dwc2,dr_mode=host` in the
`[pi02]` section of `/boot/config.txt` and remove `/boot/usb.dev`. The USB
role can also be switched live from Settings → USB.

On the LicheeRV Nano the single USB-C port is also the CDC-NCM LAN (DWC2
peripheral), so it cannot host a USB modem; use the UART HaLow link below.

## HaLow modem — SLIP wiring

Wire the RNode HaLow modem to the board's hardware UART with three 3.3 V-TTL
lines; TX and RX cross over:

| LicheeRV Nano-e / Duo S / Module 01 pad | Function | Wire to modem |
|------------------------------------------|----------|---------------|
| `GPIOA28` | UART1_TX | RX |
| `GPIOA29` | UART1_RX | TX |
| `GND`     | ground   | GND |

| Pi Zero 2 W pin | Function | Wire to modem |
|-----------------|----------|---------------|
| pin 8 (`GPIO14`)  | UART_TX | RX |
| pin 10 (`GPIO15`) | UART_RX | TX |
| any `GND`         | ground | GND |

The SG200x boards clock UART1 for an exact **2 Mbaud**; the Pi uses
`/dev/serial0` at **2 Mbaud** as well (Bluetooth is disabled so the PL011
is free). The CV1800B Milk-V Duo tops out at **1.5 Mbaud** — set both ends
to `1500000` there. Enable the link from the portal (*Reticulum tab →
HaLow modem (SLIP)*); the page adds the modem's `TCPClientInterface`
(port **4242**) to the rnsd config automatically and restarts rnsd.

## Time over Reticulum

The image ships [`rns-time`](https://github.com/wdunn001/rns-time) as
`python-rns-time` and runs it from `/etc/init.d/S88rnstime` (after `rnsd`).
The client polls one or more mesh time servers and steps the clock when
ordinary NTP has not synced it; NTP always wins, so this is a fallback, not
a second stepper. Two properties make it usable on a box with no real clock:

- the RNS Link is encrypted and authenticated **to the server's destination
  hash**, so the time cannot be spoofed by whoever answers a path request;
- the NTP-style 4-timestamp exchange cancels the path delay; expect a few
  milliseconds over TCP/LAN and well under a second over LoRa.

The default config (`/etc/rnsbox/rnstime.conf`) enables the client against
the reference minimal server `34709d9ce5c4236a421b04864b829fb1`:

```ini
enabled=yes
servers=34709d9ce5c4236a421b04864b829fb1
interval=300
step_threshold=0.5
serve=no
upstream=pool.ntp.org,time.google.com
```

Settings → Time edits the same file (server hashes, retry interval, step
threshold, and *also serve time to the mesh*). `serve=yes` runs the minimal
server too: it queries the listed upstream NTP servers, corrects its own
clock offset in the served timestamps (so its local clock need not be
right), announces its destination hash in `/run/rnstime.destination`, and
goes into honest stratum-16 holdover without announcing when no fresh
upstream sample exists. The client writes `source=rns` to `/run/clock-source`
and the RTC when it steps, sharing `S45ntpsync`'s `/run/ntpsync.step.lock`.

Deploying the server on any always-on Linux host with `rnsd`:

```bash
# systemd unit from the rns-time repository
sudo mkdir -p /opt/rns-time /var/lib/rns-time
sudo cp -r rns_time setup.py README.md /opt/rns-time/
sudo cp rnstime-server.service /etc/systemd/system/
sudo systemctl enable --now rnstime-server
journalctl -u rnstime-server -n 5     # prints the destination hash
```

No chrony, GPS or root clock privileges are needed; it joins the running
`rnsd` shared instance. **Back up `/var/lib/rns-time/identity`**: its hash is
the trust anchor clients are configured with, and a new identity means a new
destination hash. On a fresh host, one command does the whole install
(it detects the rnsd config directory and a Python with RNS, or creates a
venv):

```bash
sudo scripts/install-rns-time-server.sh --ntp pool.ntp.org,time.google.com
```

See `package/python-rns-time/src/README.md` for the full protocol and client
options.

## Repository layout

```
build.sh          one-command builder: ./build.sh <board|all>
board/<board>/    per-board files: DTS/fragments, boot config, rootfs
                  overlay (init scripts, /etc), post-image assembly
configs/          base.config + boards/<board>.config (source of truth);
                  rnsbox_<board>_defconfig is generated from them
package/          shared packages: rnsbox-portal, python-rns, python-lxmf,
                  python-nomadnet, python-rns-time, python-ble-reticulum …
                  — one copy for every board
scripts/          gen-defconfigs.sh, run/test-pc-vm.sh,
                  install-rns-time-server.sh
.github/          CI: matrix-build every board on GitHub Actions + Releases
LICENSE           MIT (the RNSBox delta)
README.md         this file
```

**Adding a board:** a defconfig delta in `configs/boards/<board>.config` +
`board/<vendor>/<board>/` (copy the shape from an existing board; keep board
differences in configs and data files, not in code forks) + one entry in the
`build.sh` board map and one line in the CI matrix. Then run
`scripts/gen-defconfigs.sh` to produce `configs/rnsbox_<board>_defconfig`.
Shared selections live in `configs/base.config`; a new shared package goes to
`package/<name>/` once and is available to every board.

## Default credentials

| Service | User  | Password |
|---------|-------|----------|
| Web UI  | admin | admin    |
| SSH     | admin | admin    |

Root login is disabled; `admin` is the operator account and has passwordless
sudo (`admin ALL=(ALL) NOPASSWD: ALL`), the one privileged path for SSH and
the browser Console. Change the password on first use (Settings → Change
admin password; it updates both the web and Unix accounts). The web password
is stored hashed (pbkdf2) in `/etc/rnsbox/auth.json` after first login, and
a lost password can be reset from the microSD card (`/boot/reset-admin`).

## Licensing

- The **RNSBox code** (the `rnsbox-portal` app, init scripts, Buildroot
  package recipes, build scripts and configuration) is released under the
  **MIT License** — see [LICENSE](LICENSE).
- The **upstream BSPs** (Linux kernel, Buildroot, Sipeed / CVITEK /
  Raspberry Pi Foundation sources) remain under their own respective
  licenses.
- The bundled Reticulum client applications are **downloaded at build
  time**, not redistributed here. They carry their own licenses, some of
  which are non-commercial (e.g. Sideband, CC BY-NC-SA) or copyleft (e.g.
  Ratspeak, AGPL-3.0); review them before redistributing any built image.

## Support RNSBox

RNSBox is free and open source (MIT). If it's useful to you, you can support
ongoing development, test hardware, and hosting for the Reticulum testnet
uplinks with a crypto donation. The web UI also has a **Donate** page
(sidebar → Donate) with a scannable QR code for each wallet.

| Coin | Address |
| --- | --- |
| Bitcoin (BTC) | `bc1q559qfr8nlqr2s6p07hgm03x357mncydehntdmlj7hu2qaud8wkqsgycagh` |
| Ethereum (ETH) | `0x5bc9b408d67c4b8294290e1dd281526be5913864` |
| Solana (SOL) | `HjpiqhDdLd3p2ZcutYFptjX9TFiNYMWFGZUGVQnfesxM` |
| Litecoin (LTC) | `ltc1qaatf3kken6peg8z6s840w4kjad643y9gptt9775zgwkhpzuyxl3qjrpss2` |
| Dogecoin (DOGE) | `9umb1Mqvq5bYg7AG8fFghvzsFHZo9fnBmf` |
