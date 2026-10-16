# SG2000 Duo Module 01 mainline port — NOTES (instance 3)

Goal: BR2_EXTERNAL board sg2000-duomodule01-mainline, modeled on cv1800b-mainline.
Branch: sg2000-mainline2 (push only this). NEVER touch C: worktree or main.

## Instance log (append after every significant step)
- [t0] Cloned /mnt/c/.../rnsbox -> ~/sg2000-mainline-repo. HEAD main=5e9c52c. Branch sg2000-mainline2 created (de78ef6).
- [t1] Studied sample + vendor (facts below). Commits: 64ced1c (vendor facts), ec554de (skeleton).
- [t2] SKELETON COMMITTED (ec554de):
  - configs/rnsbox_sg2000_duomodule01_mainline_defconfig
  - board/sg2000-mainline/{fip.bin+sha256, boot.its, genimage.cfg, post-image.sh,
    linux-sg2000-rnsbox.config, README.fip.md, busybox.fragment, rootfs-overlay
    (cv1800b copy + NEW S02klog: dmesg -n 7), linux-patches/0001-...evb-add-board.patch}
  - build.sh: BOARDS + case sg2000-duomodule01-mainline (BUILD_STYLE=external,
    same buildroot pin 679b9ead) + rnsbox_build_sg2000_duomodule01_mainline + dispatch.
  - NOT in CI matrix (.github/workflows/build.yml untouched) — bring-up board.
- [t3] DRY VALIDATION all green:
  - board dts dtc-compiles against real v7.3-rc7 tree (~/sg2000-work/evb.dtb);
  - kernel kconfig: olddefconfig against v7.3-rc7 resolves every symbol,
    allowlist is hermetic (0 symbols default-on beyond the file);
  - buildroot defconfig parses into ~/rnsbox-work/upstream/out/sg2000-duomodule01-mainline/.config;
  - boot.its FIT assembles + dumps via host mkimage (default config set, Image.gz 3.87 MiB).
- [t4] Owner build finished (BUILD-EXIT=0 at ~00:45). Full image build started.
- [t5] **FULL BUILD SUCCESS: BUILD-EXIT=0, first try.** ~45 min end-to-end in a clean
  ODIR (~/rnsbox-work/upstream/out/sg2000-duomodule01-mainline), BR2_JLEVEL=3.
  Verified artifacts in images/:
  - rnsbox-sg2000-duomodule01-mainline-lite.img (553 MB = 16MB FAT + 512MB ext4)
  - Image 7.76 MB / Image.gz 3.88 MiB (fits the 16 MB UIMAG fatload buffer)
  - sg2000-duomodule01-evb.dtb: model "Milk-V Duo Module 01 Evaluation Board",
    uart0/sdhci0 pinmux, OpenSBI region@80000000 no-map present
  - boot.sd FIT: Default Configuration = config-sg2000_duomodule01_sd, kernel+fdt
  - built kernel .config: ARCH_SOPHGO=y, PINCTRL_SOPHGO_SG2000=y, CLK_SOPHGO_CV1800=y
- [t6] Fixed en route: the drvfs-source clone lost the +x bit on build.sh
  (exit 126 "Permission denied"); chmod + re-committed the mode on the branch.
- [t7] Pushed sg2000-mainline2 to https://github.com/I-AM-ENGINEER/rnsbox.git.

## Vendor sg2000_duomodule01_sd facts (WSL ~/rnsbox/LicheeRV-Nano-Build-duo01)
- memmap.py: DRAM 0x80000000 size 512MB; OPENSBI 512K @0x80000000 (FDT @0x80080000);
  UIMAG_ADDR = DRAM+24MB = 0x81800000 (u-boot fatload buffer for boot.sd, 16MB);
  u-boot SYS_TEXT_BASE 0x80200000 (relocates to top before bootm).
- u-boot 2021.10 TARGET_CVITEK_CV181X, mars-asic.h: bootcmd = "run loadenvcmd; run sdboot || run sdbootauto";
  sdboot = setenv bootargs ${reserved_mem} ${root} ${mtdparts} console=$consoledev,$baudrate $othbootargs;
  fatload mmc 0 0x81800000 boot.sd; bootm ${uImage_addr};  <-- PLAIN bootm, NO #config suffix!
  => FIT MUST carry `default = "config-sg2000_duomodule01_sd"` (u-boot 2021.10 w/o MULTI_DTB_FIT
     resolves bare bootm via /configurations/default; same lesson as main f278299+5e9c52c).
- bootargs = "root=/dev/mmcblk0p2 rootwait rw console=ttyS0,115200 earlycon=sbi riscv.fwsz=0x80000 release loglevel=0"
  (RELEASE build => loglevel=0! workaround: overlay S02klog does `dmesg -n 7` early in boot.)
- fip.bin source: install/soc_sg2000_duomodule01_sd/fip.bin (394240 B), sha256 87e8583d...
- Board dir: build/boards/sg200x/sg2000_duomodule01_sd/ (dts_riscv/, memmap.py, partition sd: BOOT 80960KB, ROOTFS 1638400KB).

## Mainline v7.3-rc7 facts (real tree at ~/rnsbox-work/upstream/out/milkv-duo-cv1800b-mainline/build/linux-v7.3-rc7)
- Full SG2000 SoC support UPSTREAM: arch/riscv/boot/dts/sophgo/sg2000.dtsi
  (sg2000-clk + sg2000-pinctrl + sg2000-plic/clint over cv180x/cv181x dtsi),
  drivers/clk/sophgo clk-cv1800.c has "sophgo,sg2000-clk", pinctrl-sg2000.c,
  board dts sg2000-milkv-duo-s.dts + sg2002-milkv-duo256m.dts exist.
- cv18xx dtsi nodes keep cv1800b-* compatibles on sg2000 (dwcmshc, dwmac, dw-apb-uart,
  usb, saradc, rtc) => the cv1800b kernel config drivers all match; only pinctrl differs.
- SoC dtsi memory: 0x80000000 512MB (matches vendor memmap). osc 25MHz.

## Key facts
- Sample: configs/rnsbox_milkv_duo_cv1800b_mainline_defconfig, board/milkv-duo/
- Vendor ref (read-only): ~/rnsbox/milkv-duo-01-arm (arm), ~/rnsbox/LicheeRV-Nano-Build-duo01 (RV, sg2000 board)
- External flow layout: buildroot clone ~/rnsbox-work/upstream/buildroot-679b9ead (shared),
  dl cache ~/rnsbox-work/upstream/dl (shared), per-board ODIR ~/rnsbox-work/upstream/out/<board>.
  host-tools NOT needed for external style (buildroot builds its own toolchains).
- Heavy builds: BR2_JLEVEL=3 pinned in defconfig (owner's machine under load);
  check /mnt/d/Downloads/rnsbox-debug/local-build-rv.log for BUILD-EXIT before big builds.
- Main history lessons (4251df2..6f11ffd + newer): FPU allowlist fix, RISCV_ISA_FALLBACK,
  NF_CONNTRACK_MARK, getty on ttyS0, Bootlin riscv64 lp64d GLIBC + BR2_RISCV_ISA_RVC required,
  HOST_DTC + UBOOT_TOOLS_FIT_SUPPORT for mkimage, BR2_JLEVEL=3, FIT default configuration.

## What to test on hardware (for the owner)
1. SD image rnsbox-sg2000-duomodule01-mainline-lite.img -> EVB, UART0 115200:
   expect OpenSBI -> u-boot -> FIT bootm -> mainline console.
2. Console is SILENT until userspace S02klog (vendor loglevel=0): earlycon still prints
   the pre-boot log; after ~2 s the kernel spigot opens.
3. mmc: root=/dev/mmcblk0p2 baked in vendor bootargs (must match genimage layout — it does).
4. If SD unstable at HS (vendor 5.10 lesson fdf8a18 capped &sd at 25 MHz on sg200x):
   add max-frequency = <25000000> to &sdhci0 in the board dts patch and rebuild.
5. USB gadget: usb.ncm flags on /boot p1, S10usbdev builds CDC-NCM (dr_mode=peripheral in dts).
