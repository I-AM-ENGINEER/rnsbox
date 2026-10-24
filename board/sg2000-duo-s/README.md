# Milk-V Duo S (SG2000) — Linux 7.3

Linux v7.3-rc7 already supports the Duo S: internal EPHY Ethernet,
eMMC (`&emmc`), SD, the AIC8800D80 on SDIO1 with a GPIOA15 pwrseq, and
USB.  This target adds the USB gadget role (CDC-NCM portal) through
`linux-patches/0001-*`.

The shared `board/common/linux-patches/` directory adds the CV18xx
watchdog and thermal sensor to the SoC dtsi and corrects the SDHCI
"core" clock to the real card clock (CLK_SD0/SD1/EMMC), so microSD and
SDIO run at the requested rate instead of one third of it.

Boot chain: vendor `fip.bin` (built from the Sipeed LicheeRV-Nano-Build
tree, board `sg2000_duo_sd`, sha256 in `fip.bin.sha256`), our FIT
(`Image.gz` + `sg2000-milkv-duo-s.dtb`), then the ext4 rootfs on p2.
The WiFi module is the same AIC8800D80 as on the EVB; use the shared
`board/sg2000/rootfs-overlay` bring-up (WL_REG_ON = GPIOA15).
