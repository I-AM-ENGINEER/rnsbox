# fip.bin — provenance

Vendor first-stage boot blobs for the Milk-V Duo 64M (CV1800B), reused
UNCHANGED by the mainline flow: BootROM -> fip.bin (fsbl + opensbi + u-boot
2021.10) -> boot.sd (our FIT). Only boot.sd differs from the vendor flow;
everything below it stays field-tested vendor code.

- built by: LicheeRV-Nano-Build @ sipeed d4003f15 + the rnsbox
  `milkv-duo-cv1800b` patch series (workdir
  `~/rnsbox-work/LicheeRV-Nano-Build-cvitek-universal`, board
  `cv1800b_sophpi_duo_sd`, `build_uboot` stage output
  `install/soc_cv1800b_sophpi_duo_sd/fip.bin`).
- sha256: see fip.bin.sha256 (post-image.sh refuses a mismatch).
- u-boot facts baked into this blob (board/milkv-duo/post-image.sh and the
  FIT must respect them):
  - `fatload mmc 0 0x81400000 boot.sd; bootm 0x81400000#config-cv1800b_sophpi_duo_sd`
    — the FIT config MUST be named `config-cv1800b_sophpi_duo_sd`;
  - bootargs: `root=/dev/mmcblk0p2 rootwait rw console=ttyS0,115200
    earlycon=sbi riscv.fwsz=0x80000 loglevel=9`;
  - mainline Image load/entry: 0x80200000 (2MB-aligned; the vendor kernels
    load at 0x80080000, which mainline riscv does not accept).

To re-ship a newer blob: copy it over fip.bin, regenerate
`fip.bin.sha256` (`sha256sum fip.bin | awk '{print $1"  fip.bin"}' >
fip.bin.sha256`) and update this file.
