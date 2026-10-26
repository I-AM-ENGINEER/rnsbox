# Milk-V Duo Module 01 EVB (SG2000) — ARM64 (Cortex-A53), Linux 7.3

Second-build variant of `board/sg2000/` for the same hardware, used to
compare RISC-V and ARM64 performance on one board. The rv64 image boots the
C906; this one boots the Cortex-A53 with the identical userspace.

Boot chain: the vendor ARM64 `fip.bin` from the SG2000 vendor BSP (Sipeed
LicheeRV-Nano-Build tree, board `sg2000_duo01evb_arm64_sd` — the Duo Module
01 EVB; sha256 in `fip.bin.sha256`). The blob is the same one the Duo S
ARM64 variant ships; `fsbl`/u-boot are identical across the two EVBs here,
only our FIT differs (`sg2000-duomodule01-evb.dtb`). The FIT's u-boot config
name stays `config-sg2000_duo01evb_arm64_sd`: the vendor blob looks for its
board-dir name, not ours.

`linux.config` + `linux-sbc.fragment` + `linux-patches` are shared verbatim
with `board/sg2000-duo-s-arm64` (same SoC, same mainline 7.3 arm64 flow);
the rv64-specific pieces stay in `board/sg2000/`.
