# RNSBox — a Reticulum router

RNSBox turns a small, USB-powered single-board computer into a
[Reticulum](https://reticulum.network/) router and transport node with an
OpenWrt-style web admin UI. One repository serves every supported board —
currently the **Sipeed LicheeRV Nano-e** (SG2002, RISC-V) and the
**Raspberry Pi Zero 2 W** — with the router software shared and only the
board files differing.

The repository is a Buildroot **external tree** plus, for boards whose
vendor only ships a BSP fork, a git-am patch series — so it contains only
the RNSBox delta; the kernel, Buildroot and vendor sources come from
upstream and are fetched at build time.

- **LicheeRV Nano-e:** [`sipeed/LicheeRV-Nano-Build`](https://github.com/sipeed/LicheeRV-Nano-Build) at `d4003f15b` via the 23 patches in [`patches/licheerv-nano/`](patches/licheerv-nano)
- **Raspberry Pi Zero 2 W:** [`buildroot/buildroot`](https://github.com/buildroot/buildroot) 2026.02.3 LTS at `679b9ead` (no patches — the board builds straight from this repo as an external tree)

## Quick start

One command clones the pinned upstream base and builds:

```bash
git clone https://github.com/Smit1237/rnsbox
cd rnsbox
./build.sh licheerv-nano lite   # Sipeed LicheeRV Nano-e image (~217 MB)
./build.sh rpi0-2w lite         # Raspberry Pi Zero 2 W image (~433 MB)
./build.sh all lite             # every board, sequentially
# dvd instead of lite adds a read-only disc of the latest Reticulum clients
```

The finished image lands under `LicheeRV-Nano-Build/install/soc_sg2002_licheervnano_sd/images/`
(LicheeRV) or `~/rnsbox-work/upstream/out/rpi0-2w/images/` (Pi). Flash it with
`dd` (or a tool like balenaEtcher) to a microSD and boot the board.

> **First build is long.** The LicheeRV build compiles a Rust host toolchain from
> source (needed for `python-cryptography` on `riscv64-musl`), so the first run
> takes roughly 40 minutes on a typical machine; the Pi build compiles a whole
> distro (kernel, OpenSSL, Python) from scratch. Later builds are incremental.

Build host: a Linux machine set up for Buildroot (the upstream repo's
`host/ubuntu` container works; WSL2 also works). `./build.sh` needs
`/usr/sbin` on `PATH` for genimage's `mkdosfs`; it handles this itself.

## What it does

- **`rnsd`** (Reticulum, always the latest release at build time) runs as the
  long-lived service from every boot — no clock gate; the clock syncs in the
  background once a WAN exists (or seeds from your browser). Transport node
  with a `TCPServerInterface` on `0.0.0.0:4242`, an `AutoInterface` on the LAN,
  and two public RNS-testnet uplinks preconfigured.
- **USB gadget = LAN.** The board presents a CDC-NCM network interface, and the
  WiFi hotspot joins it as a second port of one bridged LAN
  (`br-lan = 10.42.0.1/24`, dnsmasq DHCP + DNS). The DVD build additionally
  exposes a read-only mass-storage "disc" pre-loaded with Reticulum client apps
  for a zero-download quick start.
- **WAN**, DHCP or static, with automatic uplink selection (wired where
  present, else the WiFi station), NAT masquerade plus per-rule port forwarding
  and open-port management.
- **`rnsbox-portal`** — a compact C++/CGI admin UI (served by uhttpd, ~0
  resident RAM) on `http://10.42.0.1/` for network,
  Reticulum, WiFi and system settings — including setting the clock from your
  browser and configuring NTP servers, handy on a board with no RTC (an
  optional DS3231 keeps time powered-off where wired). It is the
  single source of truth for the generated `nftables` ruleset (boot and
  live-apply both call the same module).
- Optional **NomadNet** LXMF / pages node (off by default) and **WiFi**
  STA/AP support where the board has a radio (AIC8800 on the Nano-e *W*,
  brcmfmac on the Pi).
- Optional **SLIP-over-UART link to an external WiFi-HaLow (RNode) modem** — a
  [RNode_Halow_Firmware](https://github.com/I-AM-ENGINEER/RNode_Halow_Firmware)
  bridge — for long-range sub-GHz Reticulum over a 3-wire serial link, with the
  modem's own web UI reverse-proxied through the portal login. Off by default.
- Optional **LXMF propagation node** (`lxmd`, off by default) for
  store-and-forward Reticulum message routing.

The design goal throughout is a minimal OS: the stock board middleware
(camera / display / audio / NPU / codec) is stripped so nearly all of the
board's RAM is available to Linux and the router data plane.

## HaLow modem — SLIP wiring

Wire the RNode HaLow modem to the board's hardware UART with three 3.3 V-TTL
lines; TX and RX cross over:

| LicheeRV Nano-e pad | Function | Wire to modem |
|---------------------|----------|---------------|
| `GPIOA28`  | UART1_TX | RX            |
| `GPIOA29`  | UART1_RX | TX            |
| `GND`      | ground   | GND           |

| Pi Zero 2 W pin | Function | Wire to modem |
|-----------------|----------|---------------|
| pin 8 (`GPIO14`)  | UART_TX | RX            |
| pin 10 (`GPIO15`) | UART_RX | TX            |
| any `GND`         | ground | GND           |

On the Nano-e set **both** ends to `1500000` baud — its UART tops out at
1,562,500, below the modem's 2 Mbaud default. On the Pi use `/dev/serial0`
at the modem's **2 Mbaud** default (Bluetooth is disabled so the PL011 is
free). Enable the link from the portal (*Reticulum tab → HaLow modem (SLIP)*),
then point a `TCPClientInterface` at the modem on **port 8001**.

## Repository layout

```
build.sh          one-command builder: ./build.sh <board> [lite|dvd|all]
board/<vendor>/   per-board files: kernel fragments, boot config, rootfs
                  overlay (init scripts, /etc), image assembly
configs/          one defconfig per board
package/          SHARED packages: rnsbox-portal, python-rns, python-lxmf,
                  python-nomadnet — one copy for every board
scripts/          lite/dvd packaging + client-disc helpers
patches/          the git-am series (vendor-BSP boards only, e.g. licheerv-nano)
.github/          CI: matrix-build the images on GitHub Actions + Releases
LICENSE           MIT (the RNSBox delta)
README.md         this file
```

**Adding a board:** `configs/rnsbox_<board>_defconfig` + `board/<vendor>/<board>/`
(copy the shape from an existing board; keep board differences in configs,
not in code forks) + a small entry in `build.sh` + one line in the CI matrix.
A new shared package goes to `package/<name>/` once and is instantly
available to every board.

## Hardware

- Sipeed LicheeRV Nano-e (no radio) or Nano-e **W** (AIC8800 WiFi). One image
  serves both; WiFi bring-up is a clean no-op where there is no radio.
- Raspberry Pi Zero 2 W (512 MB, aarch64; onboard WiFi/BT — STA / AP /
  concurrent AP+STA; WAN uplink is the WiFi station). Optional DS3231 RTC on
  i2c1: SDA GPIO2 (pin 3), SCL GPIO3 (pin 5).
- microSD: the DVD build's size tracks the current client releases (~3.3 GB
  now; use a card comfortably larger, e.g. 8 GB); the lite builds are ~217 MB
  (Nano-e) and ~433 MB (Pi). The rootfs auto-grows to fill the card on first
  boot.

## Default credentials

| Service | User  | Password |
|---------|-------|----------|
| SSH     | root  | admin    |
| Web UI  | admin | admin    |

Change these on first use. The web password is stored hashed (pbkdf2) in
`/etc/rnsbox/auth.json` after first login; the session-signing key is generated
at runtime and never stored in the source tree.

## Applying the patches by hand

Only the vendor-BSP boards carry patches. For the LicheeRV Nano-e:

```bash
git clone https://github.com/sipeed/LicheeRV-Nano-Build.git
cd LicheeRV-Nano-Build
git checkout d4003f15b35d43ad4842f427050ab2bba0114fa5
git clone --depth=1 https://github.com/sophgo/host-tools host-tools
git am /path/to/rnsbox/patches/licheerv-nano/*.patch
source build/cvisetup.sh && defconfig sg2002_licheervnano_sd && build_all
./build-rnsbox.sh lite      # or: ./fetch-clients.sh && ./build-rnsbox.sh dvd
```

The Raspberry Pi Zero 2 W needs none of this — `build.sh rpi0-2w` drives
pinned Buildroot + this repo directly.

## Licensing

- The **RNSBox code** (the `rnsbox-portal` app, init
  scripts, Buildroot package recipes, build scripts and configuration) is
  released under the **MIT License** — see [LICENSE](LICENSE).
- The **upstream BSPs** (Linux kernel, Buildroot, Sipeed / CVITEK / Raspberry
  Pi Foundation sources) remain under their own respective licenses.
- The bundled Reticulum client applications are **downloaded at build time**,
  not redistributed here. They carry their own licenses, some of which are
  non-commercial (e.g. Sideband, CC BY-NC-SA) or copyleft (e.g. Ratspeak,
  AGPL-3.0); review them before redistributing any built image.

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
