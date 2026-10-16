# SG2000 Duo Module 01 mainline port — NOTES (instance 3)

Goal: BR2_EXTERNAL board sg2000-duomodule01-mainline, modeled on cv1800b-mainline.
Branch: sg2000-mainline2 (push only this). NEVER touch C: worktree or main.

## Instance log (append after every significant step)
- [t0] Cloned /mnt/c/.../rnsbox -> ~/sg2000-mainline-repo. HEAD main=5e9c52c. Branch sg2000-mainline2 created (commit de78ef6).
- [t1] Studied sample: configs/rnsbox_milkv_duo_cv1800b_mainline_defconfig, board/milkv-duo/* (post-image.sh, genimage.cfg, boot.its, README.fip.md, linux-cv1800b-rnsbox.config), build.sh external flow (shared buildroot clone ~/rnsbox-work/upstream/buildroot-<short>, dl=upstream/dl, ODIR=upstream/out/<BOARD> per board).
- [t2] Vendor facts gathered (see below). NEXT: skeleton.

## Vendor sg2000_duomodule01_sd facts (WSL ~/rnsbox/LicheeRV-Nano-Build-duo01)
- memmap.py: DRAM 0x80000000 size 512MB; OPENSBI 512K @0x80000000 (FDT @0x80080000);
  UIMAG_ADDR = DRAM+24MB = 0x81800000 (u-boot fatload buffer for boot.sd);
  u-boot SYS_TEXT_BASE 0x80200000 (relocates to top before bootm).
- u-boot 2021.10 TARGET_CVITEK_CV181X, mars-asic.h: bootcmd = "run loadenvcmd; run sdboot || run sdbootauto";
  sdboot = setenv bootargs ${reserved_mem} ${root} ${mtdparts} console=$consoledev,$baudrate $othbootargs;
  fatload mmc 0 0x81800000 boot.sd; bootm ${uImage_addr};  <-- PLAIN bootm, NO #config suffix!
  => FIT MUST carry `default = "config-sg2000_duomodule01_sd"` (u-boot 2021.10 w/o MULTI_DTB_FIT
     resolves bare bootm via /configurations/default; same lesson as main f278299+5e9c52c).
- bootargs = "root=/dev/mmcblk0p2 rootwait rw console=ttyS0,115200 earlycon=sbi riscv.fwsz=0x80000 release loglevel=0"
- fip.bin: ~/rnsbox/LicheeRV-Nano-Build-duo01/install/soc_sg2000_duomodule01_sd/images/fip.bin (394240 B, fresh 23:46).
- Board dir: build/boards/sg200x/sg2000_duomodule01_sd/ (dts_riscv/sg2000_duomodule01_sd.dts 67 lines, memmap.py, partition sd: BOOT 80960KB, ROOTFS 1638400KB).

## Key facts
- Sample: configs/rnsbox_milkv_duo_cv1800b_mainline_defconfig, board/milkv-duo/
- Vendor ref (read-only): ~/rnsbox/milkv-duo-01-arm (BSP sipeed), ~/rnsbox/LicheeRV-Nano-Build-duo01 (RV, has sg2000_duomodule01_sd)
- host-tools: cp -al from ~/rnsbox/milkv-duo-01-arm/host-tools (do NOT download) [for patch-style; external style doesn't need host-tools — verify]
- Heavy builds: RNSBOX_NPROC=4-6; check /mnt/d/Downloads/rnsbox-debug/local-build-rv.log for BUILD-EXIT before big builds
- Target kernel: v7.3-rc7 mainline; cv1800b kernel config = allowlist style (RISCV_SBI_V01, SERIAL_OF_PLATFORM, FPU, ISA_FALLBACK...)
- Main history lessons (4251df2..6f11ffd + newer): FPU allowlist fix, RISCV_ISA_FALLBACK, NF_CONNTRACK_MARK, getty on ttyS0,
  Bootlin riscv64 lp64d GLIBC + BR2_RISCV_ISA_RVC required, HOST_DTC + UBOOT_TOOLS_FIT_SUPPORT for mkimage, BR2_JLEVEL=3.
