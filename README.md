# RNSBox

A minimal-Linux Reticulum router. One repository, many boards: a Buildroot
**external tree** — shared router software under `package/`, one defconfig +
one board directory per board, no per-board forks and no patch churn on the
mainline-buildroot side.

## Quick start

```bash
git clone https://github.com/Smit1237/rnsbox
cd rnsbox
./build.sh licheerv-nano lite   # Sipeed LicheeRV Nano-e image
./build.sh rpi0-2w lite         # Raspberry Pi Zero 2 W image (~433 MB)
./build.sh all lite             # every board, sequentially
# dvd instead of lite adds a read-only disc of the latest Reticulum clients
```

Finished images land under `~/rnsbox-work/upstream/out/<board>/images/`
(rpi0-2w) or `<workdir>/install/.../images/` (LicheeRV). Flash with `dd` or
balenaEtcher. First build is long (it compiles a whole distro: kernel,
OpenSSL, Python, `python-cryptography` needs Rust); later builds are
incremental. The shipped `rns` is always the **latest PyPI release** at
build time (sha256-verified).

Build host: a Linux machine or WSL2 set up for Buildroot.

## What it does

- **`rnsd`** (Reticulum) runs from every boot — no clock gate; NTP syncs in
  the background once a WAN exists (or seed the clock from the browser).
- **USB gadget = LAN.** CDC-NCM (`usb0`), with the WiFi hotspot as a second
  port of one bridged LAN `br-lan = 10.42.0.1/24` (DHCP + DNS by dnsmasq).
- **WAN**: automatic uplink — wired (eth0/USB adapter) or the WiFi station,
  whichever has a route; NAT, port forwards, open ports via the web UI.
- **Web admin** `http://10.42.0.1/` — a compact C++/CGI portal (uhttpd,
  ~0 resident RAM): network, WiFi, Reticulum, time/clock (optional DS3231
  RTC), in-place rnsd updates. **SSH** `root@10.42.0.1` (root/admin —
  same for the portal; WAN :22 stays firewalled until opened in the UI).
- Optional **SLIP-over-UART link to a WiFi-HaLow modem** (2 Mbaud on the
  Pi's PL011) and optional NomadNet / LXMF propagation node.

## Repository layout

```
build.sh                 one-command builder: ./build.sh <board> [lite|dvd|all]
board/<vendor>/<board>/  per-board files: kernel fragments, boot config,
                         rootfs overlay (init scripts, /etc), image assembly
configs/                 one defconfig per board
package/                 SHARED packages: rnsbox-portal, python-rns,
                         python-lxmf, python-nomadnet
scripts/                 lite/dvd packaging, client-disc helpers
patches/licheerv-nano/   git-am series (only for the vendor-BSP board)
.github/                 CI: matrix-build images + Releases
```

**Adding a board**: `configs/rnsbox_<board>_defconfig` (copy the rpi0-2w
one; RNSBox paths use the `$(BR2_EXTERNAL_RNSBOX_PATH)` prefix) +
`board/<vendor>/<board>/` (copy the overlay shape from
`board/raspberrypi/rnsbox-0-2w/`, keep board differences in configs, not
code forks) + a small entry in `build.sh` + one line in the CI matrix. A new
shared package goes to `package/<name>/` once and is instantly available to
every board.

## Hardware

- **Sipeed LicheeRV Nano-e / Nano-e W** (SG2002, riscv64) — upstream base
  [`sipeed/LicheeRV-Nano-Build`](https://github.com/sipeed/LicheeRV-Nano-Build)
  @ `d4003f15b` via `patches/licheerv-nano/`. HaLow modem on UART1
  (`/dev/ttyS1`, 1.5 Mbaud — the Nano's UART ceiling).
- **Raspberry Pi Zero 2 W** (aarch64) — official Buildroot 2026.02.3 LTS @
  `679b9ead` + RPF kernel 6.12. WiFi STA/AP/concurrent; WAN = WiFi station.
  HaLow modem on the PL011: **GPIO14 TX (pin 8) / GPIO15 RX (pin 10),
  3.3 V, `/dev/serial0` @ 2 Mbaud**; Bluetooth is disabled so the UART is
  free. Optional DS3231 RTC on i2c1: SDA GPIO2 (pin 3), SCL GPIO3 (pin 5) —
  enabled by `dtoverlay=i2c-rtc,ds3231`, harmless when absent.
- microSD: lite ≈ 433 MB (Nano-e ~217 MB); the DVD build's size tracks the
  current client releases — use a card comfortably larger. The rootfs
  auto-grows on first boot.

## License

MIT (the RNSBox delta); the board reference designs and upstream components
keep their own licenses.
