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

    linux-patches/0001-...   enable gmac0 + mdio mux in the LicheeRV DTS
    linux.config             mainline 7.3-rc7 config (same as sg2000)
    boot.its                 FIT: Image.gz + sg2002-licheerv-nano-b.dtb
    genimage.cfg             p1 FAT (fip.bin + boot.sd + gadget flags) + p2 ext4
    post-image.sh            FIT, flags, fip check, genimage
    fip.bin / .sha256        bootloader blob, provenance below
    rootfs-overlay/          tuned fstab + zram/fstrim policy

## Build

The target is a normal buildroot external-tree board:

    make O=out/licheerv-nano \
         BR2_EXTERNAL=$PWD rnsbox_licheerv_nano_mainline_defconfig
    make O=out/licheerv-nano

Output: `out/licheerv-nano/images/rnsbox-sg2002-licheervnano-mainline-lite.img`
(boot FAT at sector 1, ext4 rootfs at sector 32769, root=/dev/mmcblk0p2 is
baked into the vendor u-boot bootargs).

## fip.bin provenance

`fip.bin` (440832 bytes, sha256 in `fip.bin.sha256`) is the vendor
bootloader built from the Sipeed LicheeRV-Nano-Build tree
(`build/boards/sg200x/sg2002_licheervnano_sd`, u-boot 2021.10 + OpenSBI +
FSBL) by the same local SDK build that produced the working vendor
LicheeRV Nano image.  It is the only closed-board blob in this target;
the kernel, DTB and rootfs are built from source.

## Still to do

* WiFi/BT: the B/WE variants carry the AIC8800D80 on SDIO1.  The mainline
  LicheeRV DTS does not enable `sdhci1` yet, so `S99wifi7310` will not
  find the card on this board until the SDIO1 node/pinmux/pwrseq are
  added.  Ethernet, USB host and the SD rootfs do not depend on it.
* Hardware bring-up of this image on a physical Nano: the DTB compiles
  and its Ethernet nodes are validated, and the driver/PHY combination
  is the one proven on the EVB, but this image has not yet been booted
  on a Nano in this session.
