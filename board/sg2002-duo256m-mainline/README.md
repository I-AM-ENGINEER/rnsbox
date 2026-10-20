# Milk-V Duo 256M (SG2002) — mainline 7.3

Mainline v7.3-rc7 carries `sg2002-milkv-duo256m.dts` (SD + USB).  This
board has no WiFi and no Ethernet jack; the image is the headless router
with the SD rootfs and the USB CDC-NCM gadget.  `linux-patches/0001-*`
switches the DWC2 controller to peripheral mode.

Boot chain: vendor `fip.bin` (Sipeed LicheeRV-Nano-Build, board
`sg2002_duo_sd`, sha256 pinned), our FIT with the mainline DTB, ext4
rootfs on p2.
