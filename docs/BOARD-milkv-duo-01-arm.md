# Board: `milkv-duo-01-arm` — family-universal CVITEK ARM64 (SG2000 + SG2002)

The `milkv-duo-01-arm` build entry produces the **family-universal CVITEK
ARM64 image**: one universal aarch64 rootfs + one arm64 kernel superset +
per-board DTBs/boot blobs, covering every CVITEK chip that has a
Cortex-A53. CV1800B has no A53 and stays in the RV64 flow
(`rnsbox-cv1800b`).

## Board matrix

| board dir (`build/boards/sg200x/`) | SoC / boot CPU | DDR | package dtsi | memmap deltas vs the other board |
|---|---|---|---|---|
| `sg2000_duo01evb_arm64_sd` | SG2000, Cortex-A53 | 512 MB | `soph_asic_bga.dtsi` | ION 170 MiB, bootlogo 1800 KiB |
| `sg2002_duo_arm64_sd` | SG2002, Cortex-A53 | 256 MB | `soph_asic_qfn.dtsi` | ION 75 MiB (vendor `sg2002_duo_sd` values), bootlogo 1800 KiB |

Everything else is shared by construction:

- **Kernel**: one arm64 Image. Both boards use *byte-identical* kernel
  defconfigs (`CONFIG_ARCH_CV181X` covers both chips; the built Images
  differ only in the 28-byte build-stamp banner); only the DTBs differ.
  `build.sh` pre-seeds the second board's kernel/u-boot `O=` dirs from the
  first board's finished build, so the second `build_all` regenerates just
  its DTBs, fip and `boot.sd`. The per-board `kernel-dts` pass re-links
  `cvi_board_memmap.h` and rebuilds all DTBs with that board's memmap
  before its `boot.sd` is packed (verified: the DTB embedded in each
  board's `boot.sd` carries its own memory size — 0x1fe00000 = 510 MiB vs
  0x0fe00000 = 254 MiB — and the reclock assignments).
- **Rootfs**: one buildroot build (`cvitek_SG200X_64bit_defconfig`,
  aarch64 glibc, Arm GNU 11.3 toolchain from host-tools). The buildroot
  output is shared by every board of the workdir; the packer packs the
  same rootfs content into each board's image (each `pack_rootfs` run
  produces a fresh ext4, so bytes differ, content doesn't). Partition
  shapes live in each board dir's `partition_sd.xml` (shipped size only —
  `S99resizefs` grows the root partition at first boot).
- **Per-board blobs**: single-DTB `boot.itb`/`boot.sd` (the DTB carries
  the board's memory map), `fip.bin` (fsbl + bl31 + u-boot built with the
  board's memmap, DDR config and u-boot pinmux).

## Building

```
UPSTREAM_URL=file://$HOME/rnsbox-work/lichee-base \
HOST_TOOLS_URL=file://$HOME/rnsbox-work/lichee-build/host-tools \
RNSBOX_NPROC=2 ./build.sh milkv-duo-01-arm lite
```

Produces one image per board (the packer loops the whole family; pass
board names to pack a subset):

- `install/soc_sg2000_duo01evb_arm64_sd/images/*-lite.img`
- `install/soc_sg2002_duo_arm64_sd/images/*-lite.img`

Each image: FAT boot partition (`fip.bin`, `boot.sd`, USB-gadget flag
files) + ext4 rootfs (universal: same content for both boards).
`dvd` variant adds the read-only Reticulum-clients ISO gadget drive.
`./build-rnsbox-arm.sh {dvd|lite} [board ...]` re-packs without a BSP
rebuild.

## UART / SLIP at 2 Mbaud (both boards)

Per `local_docs/sg2000-uart-2mbit-transition.md`, implemented in the
`milkv-duo-01-arm` delta series:

- kernel: the 768 MHz disppll LUT row enabled (`clk-cv181x.c`);
- both board DTSes: `&clk` assigned-clocks reclock disppll 1188 → 768 MHz
  and `clk_cam0_200` → disppll/8 = 96 MHz in `of_clk_init()` (before any
  driver probes); `uart0`/`uart1` take `clocks = <&clk CV181X_CLK_UARTx>`
  and drop the stale `clock-frequency = <25000000>` — without the uart0
  fix the console dies 3.84x the moment the reclock lands;
- u-boot `cvi_board_init.c` muxes UART1 TX/RX onto `XGPIOA[28]`/`[29]`
  (fmux func=1, the encoding the Sipeed SG2002 boards use for these pads);
- `slip.conf` ships `baud=2000000` (exact: 96M/16/3, 0.00% error both
  ends), S31slip's fallback default too. README/portal fallback advice is
  **1000000** (also exact: quot 6) — the old `921600` suggestion is -7%
  off at 96 MHz. This is a *delta override* of the shared licheerv-nano
  `0022`, not an amend of it: the shared series stays byte-identical
  across the rnsbox repos and keeps the Nano-correct 1500000 default for
  boards without the 96 MHz reclock (Nano-e ceiling: 25 MHz/16 = 1562500);
  the delta that introduces the capability carries the default flip;
- `cpu_axi0` is deliberately not touched (FSBL runs it from FPLL/3; the
  FSBL/driver parent encodings do not map 1:1 — do not re-parent it).

Console note (vendor behavior, kept): the rootfs inittab spawns
`getty -l /etc/console_handler.sh` and that handler runs bare `login`, so
a console login asks for the username **twice** (getty consumes the
first line). Automation talking to the console must answer both prompts;
humans just type `root` twice, then the `admin` password.

## What is validated vs hardware-only

Validated (QEMU aarch64-virt, `-M virt -cpu cortex-a53`, upstream
buildroot kernel + the universal aarch64 rootfs):

- root login works (root/admin — this needed shipping a real
  `/etc/securetty`, see below);
- real `/sbin/init` completes, all 37 S-scripts run (S60routing fails
  gracefully — no eth in `-M virt`), `rnsd` starts and stays up
  (`rnsd 1.5.7`), `uhttpd` serves the portal on :80, the portal CGI
  executes (`page=overview` → 302 to the login page, exit 0) and the
  same page renders over HTTP;
- the rootfs is board-agnostic by construction: no board-specific files;
  memmap/ION live in the DTB, modules in `/mnt/system` from the same
  kernel build.

Hardware-only (needs the real boards): fsbl/DDR init for the 256 MB
SG2002, BL31 A53 entry, the actual 96 MHz reclock behavior (console must
stay alive at 115200; SLIP at exactly 2 Mbaud), SD timing, carrier
PHY/USB. Also unproven until a board is wired: whether the Milk-V Duo
carrier routes `XGPIOA[28]`/`[29]` out to a header (the pad mux is set
in u-boot either way; pads can be re-muxed at runtime via portal/devmem).

## Flashing

Both images flash identically — the .img is a raw SD card image:

```
dd if=<board>-lite.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Serial console: 115200 8N1 on the board's UART0 pads (username `root`
twice, password `admin`). First boot grows the root partition to fill
the card (S99resizefs), then `rnsd` + `uhttpd` come up; the portal is on
port 80 over the USB-CDC-NCM gadget link (`usb0`), SLIP to the HaLow
modem is configured in portal → Reticulum → *HaLow modem (SLIP)*.

## Where things live (BSP delta series `patches/milkv-duo-01-arm/`)

- `0001` sg2000_duo01evb_arm64_sd board dir; `0002` the aarch64 glibc
  buildroot defconfig; `0003` the 64bit ramdisk/rootfs overlay;
  `0004` overridable -j + buildroot toolchain path; `0005` the packer;
  `0006` THEAD LDFLAGS drop (regenerated to apply to the pristine series);
- `0007` the 768 MHz disppll LUT unlock; `0008` the sg2002_duo_arm64_sd
  board dir (256 MB memmap, QFN dtsi, byte-identical kernel defconfig —
  the superset kernel); `0009` the uart reclock fix for the sg2000 dts +
  u-boot UART1 pinmux; `0010` SLIP default 2000000 + README fallback
  1000000 (delta override of the shared 0022); `0011` the family packer
  (packs both boards per run); `0012` a real `/etc/securetty` (the rootfs
  shipped an empty one — busybox treats empty as "every tty secure";
  the explicit list documents intent and covers qemu's ttyAMA0);
  `0013` the RNSBox package block (PYTHON_RNS/LXMF/NOMADNET +
  RNSBOX_PORTAL) in the 64bit defconfig — the 0002 seed predated it and
  the arm64 rootfs shipped without rnsd/portal.

Repo-side counterparts: `build.sh` loops `RNSBOX_DEFCONFIG_BOARDS`,
pre-seeds the second board's kernel/u-boot `O=` dirs, and (critical)
restores the four `source "package/..." Config.in` lines that the
un-vendoring BSP patch (0024) removed — without them kconfig silently
dropped the rnsbox packages from EVERY patch-series flow build,
including the riscv64 Nano; the portal got a family-neutral SLIP card
and a 4 Mbaud sanity cap (real ceilings are per board).

Vendor-SDK context: Sipeed ships only ARM32 A53 board dirs
(`sg2002_licheea53nano_sd` is `dts_arm`); the arm64 machinery
(`soph_base_arm.dtsi`, `SDK_VER=64bit`) is present but unused by any
vendor board — the 64bit buildroot defconfig and the 64bit ramdisk
overlay had to be authored by this series.
