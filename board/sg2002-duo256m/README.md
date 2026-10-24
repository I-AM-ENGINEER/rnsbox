# Milk-V Duo 256M (SG2002) — Linux 7.3

Linux v7.3-rc7 carries `sg2002-milkv-duo256m.dts` (SD + USB).  This
board has no WiFi and no Ethernet jack; the image is the headless router
with the SD rootfs and the USB CDC-NCM gadget.  `linux-patches/0001-*`
switches the DWC2 controller to peripheral mode and enables saradc; the
upstream user LED (`porte 2`, heartbeat) is activated by the kernel
config.

The shared `board/common/linux-patches/` directory adds the CV18xx
watchdog and thermal sensor and fixes the SDHCI card-clock order.

Boot chain: vendor `fip.bin` (Sipeed LicheeRV-Nano-Build, board
`sg2002_duo_sd`, sha256 pinned), our FIT with the DTB, ext4
rootfs on p2.
