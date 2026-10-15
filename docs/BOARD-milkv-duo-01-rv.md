# Board: Milk-V Duo Module 01 (evaluation board) — RISC-V

`milkv-duo-01-rv` builds the RNSBox image for the **Milk-V Duo Module 01**
evaluation board (EVB) booted the RISC-V way: Linux runs on the SG2000's
**C906B** core, the Cortex-A53 stays in reset — the same boot shape as the
Sipeed LicheeRV Nano-e, so this board reuses that entire flow. (The sibling
`milkv-duo-01-arm` board builds the alternative ARM64 boot mode of the same
module; the two images are not interchangeable.)

## Hardware summary

| | |
|---|---|
| SoC | Sophgo SG2000 (BGA SiP: C906B RISC-V + Cortex-A53 + RISC-V NPU) |
| Boot core (this image) | C906B (RV64), boot ROM → SD → fsbl → u-boot → Linux |
| DRAM | 512 MB DDR3 (`ddr3_1866_x16`, the same DDR config the Duo S uses) |
| Ethernet | internal GbE PHY (CVITEK PHY, kernel driver `CVITEK_PHY_UAPS`) |
| WiFi | Module 01-**W** variants carry an SDIO AIC8800 radio (Duo S wiring); non-W images bring it up as a clean no-op |
| Console | UART0, 115200 8N1, on the EVB header |
| Boot media | microSD (EVB boots from SD by default; no boot-switch juggling needed) |

## What the build produces

```
./build.sh milkv-duo-01-rv lite     # ~217 MB image, USB-C NCM gadget only
./build.sh milkv-duo-01-rv dvd     # + read-only Reticulum clients disc
```

Image(s) land in `install/soc_sg2000_duomodule01_sd/images/` of the BSP work
tree (`WORKDIR`, default `LicheeRV-Nano-Build-duo01`). The built lite image
is `*-lite.img` (217 MB) next to the raw `boot.vfat` / rootfs partition
images if you want to assemble a card yourself.

The image layout is identical to the licheerv-nano's: boot partition
(fat32, u-boot + boot.itb) and a rootfs partition (1.6 GB lite shape; the
rootfs auto-grows to fill the card on first boot). USB-C is a CDC-NCM
gadget LAN (`10.42.0.1/24`, dnsmasq) with the admin portal on
`http://10.42.0.1/`; `eth0` (the EVB's RJ45) is the WAN side with NAT.
Default credentials: `root`/`admin` on serial console and SSH, `admin`/`admin`
on the web UI.

## How it builds (vendor-BSP patch series)

This is the repo's second `BUILD_STYLE=patches` board. `build.sh` pins the
same upstream base as the licheerv-nano —
`sipeed/LicheeRV-Nano-Build` at `d4003f15b` — applies
`patches/milkv-duo-01/` onto it and builds into its own `WORKDIR`. The
series is the lichee 24-patch RNSBox series reworked for this board:

- `0001` adds the complete board dir `build/boards/sg200x/sg2000_duomodule01_sd/`:
  SG2000 chip pin (`CONFIG_CHIP_sg2000`), the Duo S DDR3 config, a 512 M
  `memmap.py` with the headless reclamation (FreeRTOS/ION/bootlogo regions
  returned to Linux), a `dts_riscv` board dts (uart0 console + clock fix,
  25 MHz SD tuning, SDIO wifi pin wiring for the -W module), the RNSBox
  kernel defconfig plus `CVITEK_PHY_UAPS` for the internal GbE PHY, a stock
  SG2000 u-boot SD config (with GbE LED pinmux), and the RNSBox partition
  layout.
- `0002`…`0024` are the lichee series content (headless BSP strip, USB-C
  gadget, router/NAT, portal, rnsd, wifi, …) with every
  `sg2002_licheervnano_sd`-specific file hunk moved into `0001`; the
  userspace packages themselves are untouched, so the rootfs this board
  ships is the same product as the Nano-e's.
- `0025` makes the BSP's `build-rnsbox.sh` follow `RNSBOX_BSP_BOARD` (set by
  `build.sh`) so the same packer serves both patch-style boards.
- `0026` lets `RNSBOX_NPROC` cap both buildroot's per-package `-j`
  (`BR2_JLEVEL`) and the BSP makefiles' package-level `-j` — the stock
  nproc-wide LLVM pass of `host-rust` resets RAM-capped build hosts (this
  one: WSL with a 24 GB cap). Run builds as `RNSBOX_NPROC=4 ./build.sh …`
  (or 2 on very tight hosts).

Offline builds: `UPSTREAM_URL` / `HOST_TOOLS_URL` accept `file://` clones of
the BSP base and the host-tools toolchains; a first build compiles Rust from
source (for python-cryptography), so budget hours, not minutes. Heavy cvisetup
phases are stamped (`.rnsbox-done-*` in the work tree) and resumed, so an
interrupted build does not start from zero — the image for this document was
finished by a 56-minute staged resume after the first build was interrupted
by host resets.

## What is validated vs hardware-only

There is no SG200x machine model in QEMU, so the image was validated in two
tiers:

**Tier 1 — validated under QEMU (`qemu-system-riscv64 -M virt`)**: the boot
and init path. A 6.18 `Image` (riscv defconfig: virtio-mmio/blk, ext4 and
8250 console built in; built out-of-buildroot with the prebuilt host-tools
Xuantie gcc) boots the rootfs content from a virtio-blk disk with
`console=ttyS0`. On this kernel the *real* `/etc` of the image runs: busybox
`init`, the RNSBox `inittab` flow, and the complete `rcS` S-script chain.
Evidence: `qemu-boot-rv.log` (kept out of git; regenerable).

Observed results of the validation boot (all markers green):

- kernel 6.18 boots, mounts the ext4 rootfs rw on `/dev/vda`, runs
  `/sbin/init`;
- the full S-chain executes: `S02config` (syslogd/klogd OK), `S30eth`,
  `S35iptables`, `S45ntpsync` (no default route → NTP skipped, graceful),
  `S60routing` (parses the operator config: `WAN=eth0 LAN=usb0`; without
  `nft` on the validation rootfs it takes its designed fallback:
  *"nftgen failed … keeping previous /etc/nftables.conf"*), `S81router`,
  `S84cron` (honours the rnsd auto-restart setting), `S99nettest` OK,
  `S99resizefs` (graceful without `parted`);
- `/etc/rnsbox/` operator config and `/etc/init.d/` ship as built;
- root shell reachable on `ttyS0`; `poweroff` path clean.

- re-validated after the packaging fix (edf670e): the un-vendoring patch
  had silently dropped the shared packages from patch-flow rootfs builds
  (kconfig lost the `source` lines for python-rns/lxmf/nomadnet and
  rnsbox-portal), so interim images shipped without `rnsd` — the fixed
  rootfs was extracted and verified to contain `/usr/bin/rnsd`,
  `/usr/sbin/rnsbox-portal`, python3 with RNS/LXMF/nomadnet/cryptography
  43.0.3 in site-packages, and the boot harness was re-run against its
  `/etc` with identical results (`qemu-boot-rv-fixed.log`).

**Tier 2 — hardware-only (needs the real board)**

- **The product daemon binaries themselves**: `rnsd` (python3),
  `rnsbox-portal`, `uhttpd`, `dnsmasq`, `nft`, `iptables` are dynamically
  linked against the CVITEK musl interpreter
  `ld-musl-riscv64v0p7_xthead.so.1` — the T-HEAD toolchain builds for the
  C906's **RVV-0.7.1 draft vector ABI + xthead extensions**, which mainline
  QEMU does not implement. Booting the full rootfs under QEMU panics with
  `init` killed by SIGILL inside that loader; verified by an opcode sweep
  (the app binaries themselves are clean — the interpreter is the
  hardware-boundary). On the C906B these run as designed; porting the
  userspace to a buildroot-internal musl (plain rv64gc) would make the
  daemons QEMU-verifiable at the cost of replacing the vendor libc.
- fsbl / DDR init: the `ddr3_1866_x16` parameter set and the SG2000 boot ROM
  hand-off are vendor blobs + tables only proven on real silicon.
- u-boot SD boot and the boot.itb layout on real hardware.
- Pinmux-dependent blocks: the internal GbE PHY (MDIO timing, LEDs), the SDIO
  AIC8800 radio of the -W module, the 25 MHz SD-card tuning, UART0 on the EVB
  header pinout, and the USB-C NCM gadget (needs real USB pull-ups).

In practice: init, the S-script chain and the operator config pipeline are
QEMU-validated; everything from the boot ROM up to the kernel, plus the
vendor-toolchain daemons, is the part only the real board can prove. If the
board boots to a u-boot prompt but the kernel does not come up, the suspects
are, in order: DDR config, the memmap, the dts (console/SD), then the
kernel defconfig.

## Flashing

1. Grab the image (`rnsbox-milkv-duo-01-rv-*-lite.img` from the release, or
   build it: `./build.sh milkv-duo-01-rv lite`).
2. Write it to a microSD card (4 GB is enough for lite; the rootfs grows to
   fill the card on first boot):
   ```
   xzcat rnsbox-milkv-duo-01-rv-*-lite.img > /dev/sdX    # or dd + fsync
   ```
3. Put the card in the EVB, connect a serial adapter to the UART0 header
   (115200 8N1) and optionally Ethernet; power it up. No boot-switch changes
   are needed — the SG2000 boot ROM tries SD first.
4. Log in as `root`/`admin` on the console, or plug USB-C into a host: the
   board comes up as an NCM NIC with the portal at `http://10.42.0.1/`.

## Board entry reference

| `build.sh` variable | value |
|---|---|
| `BUILD_STYLE` | `patches` |
| upstream pin | `sipeed/LicheeRV-Nano-Build` @ `d4003f15b` |
| `WORKDIR` | `LicheeRV-Nano-Build-duo01` (override-friendly) |
| BSP board dir | `build/boards/sg200x/sg2000_duomodule01_sd/` |
| buildroot defconfig | `cvitek_SG200X_musl_riscv64_defconfig` (musl, riscv64, external toolchain from `host-tools`) |
| images dir | `install/soc_sg2000_duomodule01_sd/images/` |
