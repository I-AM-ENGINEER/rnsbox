# RNSBox port to the Raspberry Pi Zero 2 W — DONE (2026-09-25)

Full port completed and validated without real hardware: built in **WSL2
(Ubuntu)**, boot-tested in **QEMU** (`-M raspi3b`, the Zero 2 W's SoC class)
and smoke-tested via `qemu-aarch64-static` chroot.

## Result

- **17-patch series** in `patches/rpi0-2w/` on top of official Buildroot
  **2026.02.3** (`679b9ead7620bbf193620d1ebf56f53c1764d37a`) + the Raspberry
  Pi Foundation kernel (pinned tarball, 6.12.61 class). `./build.sh rpi0-2w
  lite` builds end-to-end; the f_ncm TX-timer fix the Lichee series carried
  is already upstream in this kernel (verified in-source) — no kernel patch
  needed.
- **rnsbox-rpi0-2w-lite.img (~433 MB)**: boot FAT (config.txt: headless,
  gpu_mem=16, disable-bt, dwc2 peripheral; gadget flag files; pinned
  cmdline) + 400 MB ext4 rootfs that auto-grows on first boot.
- **QEMU boot validated**: full rcS — syslog/klog, S02resizefs, gadget
  composer (flags read from /boot), WAN=wlan0 default, SLIP/NTP/rnsd gating
  (rnsd correctly waits for clock-sync), nftables+dnsmasq, **uhttpd portal
  OK**, opt-ins (nomadnet/lxmd) off by default.
- **Chroot smokes**: `import RNS` + `rnsd --version` OK (needs
  BR2_PACKAGE_PYTHON3_BZIP2 — found & fixed), portal CGI renders (302 →
  login), `nftgen --wan wlan0 --lan usb0` generates a valid ruleset.

## Porting notes worth keeping

- The RPF 6.12 kernel numbers the PL011 **ttyAMA1** (mini-uart claims AMA0);
  everything user-facing uses **`/dev/serial0`** (primary-UART alias) for
  SLIP. In QEMU the serial console is `console=ttyAMA1` (test-harness only;
  the shipped cmdline has no serial console — tty1 only).
- Buildroot 2026.02 `BR2_ROOTFS_DEVICE_TABLE` must include
  `system/device_table_dev.txt` — without it /dev/console is missing and the
  whole userspace boots silently blind ("unable to open an initial console").
  The series pins this explicitly + rcS re-attaches stdout to /dev/console
  defensively.
- uhttpd/libubox (2019-era CMakeLists) need
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` under host-cmake 4.x.
- Config.in wiring anchors from older buildroot may not exist; the series
  wires python-rns/lxmf/nomadnet after `package/python3` (there is no
  python3-pip package dir anymore).
- The clients DVD (fetch-clients.sh) is host-PC installers — arch-agnostic,
  reused verbatim.

## Verify-on-hardware list (untestable in QEMU)

USB gadget NCM enumeration on a real host (dwc2 peripheral + current
limits), WiFi STA/AP/APSTA with the real brcmfmac radio (uap0 vif),
SLIP at 2 Mbaud on real GPIO, SD/power behaviour of the micro-USB OTG port.

## Decisions (user-approved, 2026-09-24)

| Question | Decision |
|---|---|
| OS arch | aarch64 (bootlin musl stable) |
| WAN role | WiFi STA (wlan0 default; USB-Ethernet adapter = optional eth0 WAN) |
| WiFi modes | off / sta / ap / sta+ap (concurrent AP+STA via `uap0` vif, co-channel) |
| AIC8800 | dropped (board-unique builds; onboard brcmfmac instead) |
| SLIP UART | hardware UART (`/dev/serial0`, GPIO14/15) at 2 Mbaud; BT disabled, no serial console |
| DVD variant | kept |
| Base | official Buildroot 2026.02.3 LTS + RPF kernel (pinned tarball) |
| CI | build-lite-rpi.yml (GH Actions) |
| Acceptance | build green in WSL + QEMU boot + chroot smokes (met) |
| Layout | multi-board (patches/<board>/ + boards/), mergeable back to main |
