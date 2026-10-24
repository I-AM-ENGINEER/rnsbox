# RNSBox — a Reticulum router

RNSBox turns a small, USB-powered single-board computer into a
[Reticulum](https://reticulum.network/) router and transport node with an
OpenWrt-style web admin UI. One repository serves every supported board —
the **Sipeed LicheeRV Nano-e**, **Milk-V Duo / Duo S / Duo 256M** and
**Duo Module 01** (SG2000/SG2002, RISC-V C906 and ARM Cortex-A53), the
**Raspberry Pi** (one universal image for Zero 2 W / 3 / 4 / 5) and the
**Orange Pi R1** (Allwinner H2+) — with the router software shared and only
the board files differing.

The repository is a Buildroot **external tree**, and there is exactly one
build flow: pinned upstream Buildroot + Linux + this repo as
`BR2_EXTERNAL`. The only vendor artifact left is the CVITEK first-stage boot
blob (`fip.bin`), which a board's post-image script writes to the boot FAT;
there is no vendor Buildroot, no per-board packer and no patch series.

- **Common base:** [`buildroot/buildroot`](https://github.com/buildroot/buildroot)
  2026.02.3 LTS at `679b9ead` + Linux.
- **CVITEK SG2000/SG2002:** DTS and drivers under the
  `board/` directories; the vendor `fip.bin` is only a boot blob.
- **Raspberry Pi, Orange Pi R1, PC/VM:** upstream Buildroot + Linux,
  no vendor blob at all.

## Quick start

One command clones the pinned upstream base and builds:

```bash
git clone https://github.com/Smit1237/rnsbox
cd rnsbox
./build.sh rpi-universal               # Raspberry Pi Zero 2 W / 3 / 4 / 5
./build.sh orangepi-r1                 # Orange Pi R1 (Allwinner H2+)
./build.sh pc-x86_64                   # PC / QEMU / VirtualBox disk
./build.sh milkv-duo-cv1800b-rv64      # Milk-V Duo 64M (C906)
./build.sh sg2000-duomodule01-rv64     # Duo Module 01 / EVB (C906)
./build.sh sg2002-licheerv-nano-rv64   # LicheeRV Nano-e (C906)
./build.sh sg2002-duo256m-rv64         # Duo 256M (C906)
./build.sh sg2000-duo-s-rv64           # Duo S (C906)
./build.sh sg2000-duo-s-arm64          # Duo S (A53)
./build.sh all                         # every board, sequentially
```

The finished image lands at
`~/rnsbox-work/upstream/out/<board>/images/rnsbox-<board>.img`. Flash it with
`dd` (or a tool like balenaEtcher) to a microSD and boot the board. CVITEK
images are still named by SoC+core (`sg2000-*` / `sg2002-*`) because the
physical rv/arm switch selects the boot core; the board key also says which
core it was built for.

> **First build is long.** Buildroot compiles the kernel, OpenSSL, Python and
> the rest of the distro from scratch. Later builds are incremental.

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
  (`br-lan = 10.42.0.1/24`, dnsmasq DHCP + DNS).
- **WAN**, DHCP or static, with automatic uplink selection (wired where
  present, else the WiFi station), NAT masquerade plus per-rule port forwarding
  and open-port management.
- **`rnsbox-portal`** — a compact C++/CGI admin UI (served by uhttpd, ~0
  resident RAM) on `http://10.42.0.1/` for network,
  Reticulum, WiFi and system settings — including setting the clock from your
  browser and configuring NTP servers, handy on a board with no RTC (an
  optional DS3231 keeps time powered-off where wired). A **Console** tab gives
  the admin shell in the browser over a real PTY, and a **NomadNet** tab runs
  the NomadNet TUI the same way; either can open full-window in its own browser
  tab. It is the single source of truth for the generated `nftables` ruleset
  (boot and live-apply both call the same module).
- Optional **NomadNet** LXMF / pages node (off by default) and **WiFi**
  STA/AP support where the board has a radio (AIC8800 on the Nano-e *W*,
  brcmfmac on the Pi). A fresh image raises the AP by default, named after
  the box (`rnsbox-xxxxxx`) with password `reticulum`; change it in the
  WiFi tab. Boards without a radio keep `mode=off`.
- Optional **SLIP-over-UART link to an external WiFi-HaLow (RNode) modem** — a
  [RNode_Halow_Firmware](https://github.com/I-AM-ENGINEER/RNode_Halow_Firmware)
  bridge — for long-range sub-GHz Reticulum over a 3-wire serial link, with the
  modem's own web UI reverse-proxied through the portal login. Off by default.
- Optional **LXMF propagation node** (`lxmd`, off by default) for
  store-and-forward Reticulum message routing.
- **BLE transport, both roles.** The box advertises a Reticulum GATT service
  and scans for phones that only advertise (Columba/Sideband). The same
  adapter optionally exposes an **RNode-over-BLE** endpoint (Nordic UART
  Service): pair the phone, pick the box as an RNode, and its packets are
  bridged into the local Reticulum transport — useful for a portable node.
  The **RNode** tab drives `rnodeconf` for attached USB modems (info, config,
  live counters, display framebuffer, Bluetooth pairing) and links the
  [RNode HaLow firmware](https://github.com/I-AM-ENGINEER/RNode_Halow_Firmware).
- **Map tab.** Reticulum interface-discovery announces that carry coordinates
  are collected by `rnsd` and drawn on a bundled offline world outline; the
  data lives on flash, the browser renders it, and nothing is fetched from
  the internet.

The design goal throughout is a minimal OS: the stock board middleware
(camera / display / audio / NPU / codec) is stripped so nearly all of the
board's RAM is available to Linux and the router data plane.

## USB modems and RNodes

The Raspberry Pi images carry the USB serial and modem drivers (`option`,
`qmi_wwan`, `cdc_acm`, `cdc_ncm`/`ecm`, `ch341`, `cp210x`, `ftdi_sio`). On a
Pi 3/4/5 plug the modem or RNode into a USB-A port. The Zero 2 W has a single
micro-USB port that the image uses as the CDC-NCM LAN gadget: to use a USB
modem there instead, set `dtoverlay=dwc2,dr_mode=host` in the `[pi02]` section
of `/boot/config.txt` and remove `/boot/usb.dev`.

On the LicheeRV Nano the single USB-C port is also the CDC-NCM LAN (DWC2
peripheral), so it cannot host a USB modem; use the UART HaLow link below.

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
free). Enable the link from the portal (*Reticulum tab → HaLow modem (SLIP)*);
the page adds the modem's `TCPClientInterface` (port **4242**) to the rnsd
config automatically and restarts rnsd.

## Repository layout

```
build.sh          one-command builder: ./build.sh <board|all>
board/<board>/    per-board defconfig inputs: DTS/fragments, boot config,
                  rootfs overlay (init scripts, /etc), post-image assembly
configs/          base.config + boards/<board>.config (source of truth);
                  rnsbox_<board>_defconfig is generated from them
package/          SHARED packages: rnsbox-portal, python-rns, python-lxmf,
                  python-nomadnet — one copy for every board
                  (package/rnsbox-portal/src/win/ builds the same portal
                  natively for Windows — see its README)
scripts/          gen-defconfigs.sh, run/test-pc-vm.sh
.github/          CI: matrix-build the images on GitHub Actions + Releases
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

## Windows (native, for development)

The portal also builds as a plain Windows executable — the same C++ routes and
pages, served by a built-in HTTP server, running on the system Python +
Reticulum instead of a router image. No VM/WSL; WiFi, routing, the PTY
consoles and the A/B updater are absent by design.

```bash
cd package/rnsbox-portal/src
./win/build-win.sh                      # MSYS2 UCRT64 -> win/build/rnsbox.exe
cd win/build && ./rnsbox.exe --setup    # finds/installs Python + Reticulum
./rnsbox.exe                            # http://127.0.0.1:8040/ (admin/admin)
```

`rnsbox.exe --install-service` (elevated) registers it as a background Windows
service. Details: `package/rnsbox-portal/src/win/README.md`.

## Hardware

- Sipeed LicheeRV Nano-e (no radio) or Nano-e **W** (AIC8800 WiFi):
  `sg2002-licheerv-nano-rv64`. One image serves both; WiFi bring-up is a
  clean no-op where there is no radio.
- Milk-V Duo 64M: `milkv-duo-cv1800b-rv64`; Duo 256M:
  `sg2002-duo256m-rv64`; Duo Module 01 on the EVB:
  `sg2000-duomodule01-rv64`; Duo S on the C906 or A53:
  `sg2000-duo-s-rv64` / `sg2000-duo-s-arm64`.
- Raspberry Pi: one universal image for Zero 2 W, 3, 4 and 5 (aarch64;
  onboard WiFi/BT on the Zero 2 W — STA / AP / concurrent AP+STA; WAN uplink
  is the WiFi station). Optional DS3231 RTC on i2c1: SDA GPIO2 (pin 3),
  SCL GPIO3 (pin 5).
- Orange Pi R1 (Allwinner H2+) and a generic PC/VM disk (`pc-x86_64`).
- microSD: one image per board (one USB-NCM gadget; no clients disc). The rootfs auto-grows to
  fill the card on first boot.

## Default credentials

| Service | User  | Password |
|---------|-------|----------|
| SSH     | admin | admin    |
| Web UI  | admin | admin    |

Root login is disabled; `admin` is the operator account and has passwordless
sudo (`admin ALL=(ALL) NOPASSWD: ALL`), the one privileged path for SSH and
the browser Console. Change the password on first use (Settings → Change
admin password; it updates both the web and Unix accounts). The web password
is stored hashed (pbkdf2) in `/etc/rnsbox/auth.json` after first login.

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
