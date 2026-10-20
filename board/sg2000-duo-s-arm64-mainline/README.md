# Milk-V Duo S (SG2000) — mainline 7.3

Mainline v7.3-rc7 already supports the Duo S on both cores: internal EPHY Ethernet,
eMMC (`&emmc`), SD, the AIC8800D80 on SDIO1 with a GPIOA15 pwrseq, and
USB.  This target adds only the USB gadget role (CDC-NCM portal) through
`linux-patches/0001-*`.

Boot chain: the vendor ARM64 `fip.bin` (built from the Sipeed LicheeRV-Nano-Build
tree, board `sg2000_duo01evb_arm64_sd`, sha256 in `fip.bin.sha256`), our FIT
(arm64 `Image` + `sg2000-milkv-duo-s.dtb`), then the ext4 rootfs on p2.
The WiFi module is the same AIC8800D80 as on the EVB; use the shared
`board/sg2000-mainline/rootfs-overlay` bring-up (WL_REG_ON = GPIOA15).
