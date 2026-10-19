# LicheeRV Nano (SG2002) — mainline 7.3

Scaffold for the Sipeed LicheeRV Nano B. The SoC is an SG2002 (same C906
core as the SG2000; the package/pinmux is the difference), and mainline
v7.3-rc7 already carries `arch/riscv/boot/dts/sophgo/sg2002-licheerv-nano-b.dts`,
so the board only needs the RNSBox build glue:

  boot.its      FIT with Image.gz + sg2002-licheerv-nano-b.dtb
  genimage.cfg  p1 boot FAT + p2 ext4 rootfs (same shape as sg2000-mainline)
  post-image.sh FIT, gadget flags, fip.bin provenance, genimage
  rootfs-overlay/etc/init.d/S05zram  zram (<900 MB RAM policy)

Remaining to make it a shipped target:

1. `board/sg2002-mainline/fip.bin` + `fip.bin.sha256` — the vendor blob for
   the LicheeRV Nano (provenance-pinned like the Duo Module 01 one).
2. A defconfig under `configs/` (kernel = mainline 7.3-rc7 + this board's
   linux.config, intree DTS `sg2002-licheerv-nano-b`) and a build.sh case.
3. Shared userspace overlay: reuse `board/sg2000-mainline/rootfs-overlay`
   (S10usbdev/S30gadget_nic/S35wifi/S60routing...) — the overlay is
   SoC-independent; keep one copy or factor it out.
4. WiFi/BT: the -W variant carries the AIC8800D80 on SDIO — reuse
   `board/sg2000-mainline/aic8800` and the S99wifi7310 bring-up unchanged
   (same SDIO1 slot, same LPO/RTC fix).
5. TMI7003C: no driver exists in our trees (nor in mainline v7.3). The
   board boots on the rails set by u-boot; adding a runtime regulator
   driver needs the vendor source (see README.aic8800.md / TMI7003C note).
