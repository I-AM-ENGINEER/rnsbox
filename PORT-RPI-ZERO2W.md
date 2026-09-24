# RNSBox port to the Raspberry Pi Zero 2 W

Working notes for the port. Everything is developed and tested without real
hardware: build inside **WSL2 (Ubuntu)**, boot-test the image in
**QEMU** (`-M raspi3b` — the Zero 2 W shares the bcm2837/2710 SoC family with
the Pi 3), plus `qemu-aarch64-static` chroot smoke tests for the rootfs
services.

## Target

- Raspberry Pi Zero 2 W: quad Cortex-A53 (BCM2710A1), 512 MB RAM,
  onboard WiFi/BT (brcmfmac), one micro-USB OTG port (power + data),
  no Ethernet, no RTC.
- Same product shape as the LicheeRV build: USB gadget CDC-NCM LAN
  (`usb0 = 10.42.0.1/24`), NAT router, `rnsd` transport node, the
  `rnsbox-portal` C++/CGI admin UI, optional NomadNet / lxmd / DVD / SLIP.

## Approach

Keep the repo's "patch series on a pinned upstream" model:

- **Upstream base:** official Buildroot (pinned LTS) with the Raspberry Pi
  Foundation kernel through Buildroot's `raspberrypi` packages — replaces the
  Sipeed/CVITEK BSP.
- **New board dir** `board/raspberrypi/rnsbox-0-2w/` (overlay + defconfig +
  genimage config), new RNSBox defconfig derived from Buildroot's
  `raspberrypi0-2w` / `raspberrypi_64` defconfigs.
- USB gadget: `dwc2` (the Pi's OTG controller) with the same libcomposite
  NCM (+ mass-storage for the DVD variant) setup as patch 0003.
- No Rust-host-toolchain dance expected: aarch64 has `musllinux_aarch64`
  wheels, and target pip upgrades of `rns`/`cryptography` work from PyPI.

## Patch porting map (23 Lichee patches -> Pi series)

| Lichee patch | Pi fate |
|---|---|
| 0001 strip BSP | n/a — Buildroot official is already headless/minimal |
| 0002 kernel defconfig | new: Pi Foundation kernel config (NCM gadget, nftables, SLIP, brcmfmac, dwc2) |
| 0003 USB gadget NCM/MS | port: same configfs layout, `dwc2` + OTG overlay on the micro-USB port |
| 0004 NTP clock discipline | port as-is (no RTC on the Pi either) |
| 0005 router NAT data plane | port: overlay paths move to the new board dir; WAN role TBD (Q2) |
| 0006 Rust host toolchain + cryptography | expected drop (aarch64 wheels) |
| 0007 buildroot prebuilt target std | expected drop |
| 0008 rnsd daemon + config | port as-is |
| 0009 portal C++/CGI UI | port as-is (rebuild natively for aarch64) |
| 0010 NomadNet opt-in | port as-is |
| 0011 AIC8800 WiFi | replaced by brcmfmac (onboard WiFi); AIC8800 drop TBD (Q4) |
| 0012 cron rnsd auto-restart | port as-is |
| 0013 clients DVD | port: fetch aarch64 client builds |
| 0014 LOCALVERSION | port (kernel naming) |
| 0015 Buildroot defconfig + wiring | new defconfig for the Pi |
| 0016 docs MIT/apply guide | port + rewrite for the new base |
| 0017 portal rnsd update check | port as-is |
| 0018 portal in-place rnsd update | port (pip wheels, aarch64) |
| 0019 rns wheel install | port as-is |
| 0020 donate page | port as-is |
| 0021 Time & Clock settings | port as-is |
| 0022 SLIP HaLow UART | port; UART choice TBD (Q5) |
| 0023 lxmd | port as-is |

## QEMU acceptance (no hardware)

What "done" means here:

1. `./build.sh lite` succeeds inside WSL2 Ubuntu.
2. Image boots to login in `qemu-system-aarch64 -M raspi3b` over the emulated
   UART; rnsd, uhttpd/portal, dnsmasq, nftables all reach their running state.
3. `qemu-aarch64-static` chroot: portal CGI renders, nft ruleset validates,
   rnsd starts and announces.
4. Known-untestable-in-QEMU (needs real hardware, listed for the release
   notes): USB gadget NCM enumeration on a real host, WiFi STA/AP with the
   onboard radio, SD/timing quirks, power/current-limit behaviour of the
   micro-USB OTG port, SLIP at 1.5 Mbaud on real GPIO.

## Open questions (blocking the build)

Listed in the PR/conversation; answers get folded back into this file.
