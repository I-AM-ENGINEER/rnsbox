# LicheeRV Nano B (SG2002) — mainline 7.3

Target for the Sipeed LicheeRV Nano B / E / WE (SG2002, C906).  Mainline
v7.3-rc7 already carries the board DTS
`arch/riscv/boot/dts/sophgo/sg2002-licheerv-nano-b.dts`; this board dir
adds the RNSBox build glue and the one missing hardware enablement.

## Ethernet (and what TMI7003C really is)

The E/WE edition has a 100M RJ45 jack wired to the **SG2002 internal
EPHY**: the SoC's `EPHY_TX_P/N` and `EPHY_RX_P/N` pairs go through AC
coupling capacitors straight to the magjack.  There is no external
MAC/PHY on the board.

The `TMI7003C` (U4) on the schematic is the **3-channel DCDC + LDO
power regulator**, section "3CH DCDC + LDO": pins VIN1/2/3, SW1/2/3,
FB1/2, outputs VDD3V3_SYS / VDD1V35_DRAM / VDD0V9_CPU.  Its EN1/2/3
straps are tied to VSYS and it has no I2C/GPIO control interface, so it
needs no Linux driver.  It is not an Ethernet device.

What *was* missing in mainline was in the DTS: `gmac0` and the MDIO mux
were left disabled.  `linux-patches/0001-*` enables both, exactly as the
Milk-V Duo S DTS does for the same internal EPHY on SG2000:

    &gmac0 { status = "okay"; };
    &mdio  { status = "okay"; };

`linux.config` already contains the needed driver stack:
`CONFIG_STMMAC_ETH`, `CONFIG_DWMAC_GENERIC`, `CONFIG_DWMAC_SOPHGO`,
`CONFIG_PHYLIB`, `CONFIG_MDIO_BUS_MUX_MMIOREG`.

The same EPHY path was verified live on the Duo Module 01 EVB under 7.3:

    stmmaceth 4070000.ethernet eth0: PHY [mdio_mux-0.0:01] driver [Generic PHY]
    stmmaceth 4070000.ethernet eth0: Link is Up - 100Mbps/Full

## Files

    linux-patches/0001-...   Ethernet, SDIO Wi-Fi, USB gadget, user LED/button, saradc
    linux.config             mainline 7.3-rc7 config (same as sg2000)
    boot.its                 FIT: Image.gz + sg2002-licheerv-nano-b.dtb
    genimage.cfg             p1 FAT (fip.bin + boot.sd + gadget flags) + p2 ext4
    post-image.sh            FIT, flags, fip check, genimage
    fip.bin / .sha256        bootloader blob, provenance below
    rootfs-overlay/          tuned fstab + zram/fstrim policy

The shared `board/common/linux-patches/` directory (prepended to
`BR2_LINUX_KERNEL_PATCH`) adds the CV18xx watchdog and thermal sensor
nodes plus the SDHCI card-clock fix for every CV1800B/SG2000/SG2002
board in this tree.

## User I/O, sensors and watchdog

The board DTS exposes the on-board blue user LED (`GPIOA14`,
`/sys/class/leds/licheerv-nano:blue:user`), the user button (`GPIOA30`,
active low, `KEY_DISPLAYTOGGLE`) and `saradc`.  The kernel config enables
`NEW_LEDS`/`LEDS_GPIO` and `INPUT_EVDEV`/`KEYBOARD_GPIO` for them.

The common SoC patch adds `snps,dw-wdt` at `0x3010000`
(`/dev/watchdog0` with `CONFIG_DW_WATCHDOG`) and the TEMPSEN sensor at
`0x30e0000` with a `soc-thermal` zone (passive 85 C, hot 95 C, critical
110 C).  The sensor needs the companion `CV1800_THERMAL` driver patch.
On this board `GPIOA15` is the bit-banged I2C bus, so it is deliberately
not used for the speaker enable or Wi-Fi power.


## Build

The target is a normal buildroot external-tree board:

    make O=out/licheerv-nano \
         BR2_EXTERNAL=$PWD rnsbox_licheerv_nano_mainline_defconfig
    make O=out/licheerv-nano

Output: `out/licheerv-nano/images/rnsbox-sg2002-licheervnano-mainline.img`
(the `-dvd` suffix appears only when that variant is built).
(boot FAT at sector 1, ext4 rootfs at sector 32769, root=/dev/mmcblk0p2 is
baked into the vendor u-boot bootargs).

## fip.bin provenance

`fip.bin` (440832 bytes, sha256 in `fip.bin.sha256`) is the vendor
bootloader built from the Sipeed LicheeRV-Nano-Build tree
(`build/boards/sg200x/sg2002_licheervnano_sd`, u-boot 2021.10 + OpenSBI +
FSBL) by the same local SDK build that produced the working vendor
LicheeRV Nano image.  It is the only closed-board blob in this target;
the kernel, DTB and rootfs are built from source.

## Wi-Fi / BT (B/WE variants)

The same AIC8800D80 as on the EVB sits on SDIO1 (`0x4320000`).  The DTS
now enables `sdhci1` with the real card clock (`CLK_SD1`, 375 MHz parent,
25 MHz cap), the SD1 pin group (`PIN_SD1_*`, function 0 as the vendor
u-boot programs it), and an `mmc-pwrseq-simple` on **GPIOA26** - the pin
the vendor `cvi_board_init` drives as WL_REG_ON.  GPIOA15 is the
bit-banged I2C bus on this board, which is why the EVB bring-up cannot
be used unchanged.

The AIC BSP takes two board knobs: `wlregon_pin=26` and
`sd1_pads_config=0` (the DT pinctrl owns the SD1 pads).  Firmware, the
LPO/RTC fix, the chip_reset sequence and the SDIO function-2 BT path are
shared with the EVB; `rootfs-overlay/root/wifi.sh` and `S99wifi7310`
carry the LicheeRV values.

## Still to do

* Hardware bring-up of this image on a physical Nano: the DTB compiles
  and its Ethernet/SDIO nodes are validated, and the driver/PHY
  combination is the one proven on the EVB, but this image has not yet
  been booted on a Nano in this session.
