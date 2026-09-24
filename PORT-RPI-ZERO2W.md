# RNSBox port to the Raspberry Pi Zero 2 W

Working notes for the port. Everything is developed and tested without real
hardware: build inside **WSL2 (Ubuntu)**, boot-test the image in
**QEMU** (`-M raspi3b` — the Zero 2 W shares the bcm2837/2710 SoC family with
the Pi 3), plus `qemu-aarch64-static` chroot smoke tests for the rootfs
services.

## Status (2026-09-24, late)

- Series authored (12 patches) on top of Buildroot **2026.02.3**
  (`679b9ead7620bbf193620d1ebf56f53c1764d37a`), in the WSL buildroot clone
  (branch `rnsbox`), regenerating into `patches/rpi0-2w/` once green:
  01 board skeleton (config.txt/cmdline/linux.fragment/defconfig),
  02 libubox+uhttpd, 03 rnsbox-portal (+Pi defaults: ttyAMA0/2 Mbaud, brcmfmac
  texts), 04 gadget (S10usbdev configfs composer + S30gadget_nic + fstab/
  inittab), 05 router data plane (WAN=wlan0 default, S02resizefs), 06 NTP,
  07 rnsd + python-rns (`?=` version for latest-tracking), 08 WiFi
  STA/AP/sta+ap (uap0 vif), 09 SLIP 2 Mbaud, 10 lxmd+nomadnet+cron,
  11 clients DVD + lite/dvd packaging, 12 docs. f_ncm TX-timer fix pending
  kernel source availability (ported next).
- Repo: multi-board layout (`patches/<board>/`, `boards/<board>.sh`),
  `./build.sh <board> [lite|dvd]`, latest-rns-from-PyPI tracking (rewrites
  package + .hash, pre-seeds dl cache, sha256-verified) for BOTH boards,
  CI `build-lite-rpi.yml`.
- Build running in WSL. QEMU boot test + chroot smoke tests next.

## Decisions (user-approved, 2026-09-24)

| Question | Decision |
|---|---|
| OS arch | aarch64 (bootlin musl stable) |
| WAN role | WiFi STA (wlan0 default; USB-Ethernet adapter = optional eth0 WAN) |
| WiFi modes | off / sta / ap / sta+ap (concurrent AP+STA via `uap0` vif, co-channel) |
| AIC8800 | dropped (board-unique builds; onboard brcmfmac instead) |
| SLIP UART | hardware UART PL011 (`/dev/ttyAMA0`, GPIO14/15) at 2 Mbaud; BT disabled, no serial console (tty1 only) |
| DVD variant | kept |
| Base | official Buildroot 2026.02.3 LTS + RPF kernel (pinned tarball) |
| CI | build-lite-rpi.yml (GH Actions) |
| Acceptance | build green in WSL + QEMU raspi3b boot + chroot smokes; USB-gadget/WiFi/SLIP-on-hardware listed as "verify on hardware" |
| Layout | multi-board (patches/<board>/ + boards/), mergeable back to main |

## Known-untestable without hardware (release-notes list)

USB gadget NCM enumeration on a real host (dwc2 peripheral mode, current
limits), WiFi STA/AP with the real radio, SLIP at 2 Mbaud on real GPIO,
SD/power behaviour of the micro-USB OTG port.
