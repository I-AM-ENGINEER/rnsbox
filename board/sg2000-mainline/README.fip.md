# fip.bin — provenance

Vendor first-stage boot blobs for the Milk-V Duo Module 01 Evaluation Board
(SG2000, C906, 512 MB DDR3), reused UNCHANGED by the mainline flow:
BootROM -> fip.bin (fsbl + opensbi + u-boot 2021.10) -> boot.sd (our FIT).
Only boot.sd differs from the vendor flow; everything below it stays
field-tested vendor code.

- built by: LicheeRV-Nano-Build @ sipeed d4003f15 + the rnsbox
  `milkv-duo-01` patch series (workdir
  `~/rnsbox/LicheeRV-Nano-Build-duo01`, board `sg2000_duomodule01_sd`,
  output `install/soc_sg2000_duomodule01_sd/fip.bin`).
- sha256: see fip.bin.sha256 (post-image.sh refuses a mismatch).
- u-boot facts baked into this blob (board/sg2000-mainline/post-image.sh
  and the FIT must respect them):
  - bootcmd: `run loadenvcmd; run sdboot || run sdbootauto` where sdboot =
    `setenv bootargs ${reserved_mem} ${root} ${mtdparts}
    console=$consoledev,$baudrate $othbootargs; mmc dev 0 && fatload mmc 0
    0x81800000 boot.sd; bootm 0x81800000;` — a BARE bootm, no #config
    suffix (CVICHIP is not defined in this SDK build), so u-boot 2021.10
    without CONFIG_MULTI_DTB_FIT picks the configuration through the
    `default` property of /configurations. The FIT MUST carry
    `default = "config-sg2000_duomodule01_sd"` (kept as the vendor
    convention for the name).
  - bootargs: `root=/dev/mmcblk0p2 rootwait rw console=ttyS0,115200
    earlycon=sbi riscv.fwsz=0x80000 release loglevel=0` — note loglevel=0:
    the vendor build defines RELEASE, so kernel messages are silenced;
    the RNSBox overlay re-raises them at boot (rootfs-overlay
    etc/init.d/S02klog). `riscv.fwsz` is ignored by mainline — the
    OpenSBI region (0x80000000..0x80080000) is reserved in the board DTS
    instead.
  - mainline Image load/entry: 0x80200000 (2MB-aligned; the vendor kernels
    load at 0x80080000, which mainline riscv does not accept). The FIT
    itself is fatloaded to 0x81800000 (memmap UIMAG_ADDR, 16 MB buffer).
  - env is CONFIG_ENV_IS_NOWHERE — no uEnv.txt can override bootargs on
    the board; the FIT content is the only lever.

To re-ship a newer blob: copy it over fip.bin, regenerate
`fip.bin.sha256` (`sha256sum fip.bin | awk '{print $1"  fip.bin"}' >
fip.bin.sha256`) and update this file.
