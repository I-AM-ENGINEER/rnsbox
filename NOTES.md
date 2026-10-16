# SG2000 Duo Module 01 mainline port — NOTES (instance 3)

Goal: BR2_EXTERNAL board sg2000-duomodule01-mainline, modeled on cv1800b-mainline.
Branch: sg2000-mainline2 (push only this). NEVER touch C: worktree or main.

## Instance log (append after every significant step)
- [t0] Cloned /mnt/c/.../rnsbox -> ~/sg2000-mainline-repo. HEAD main=5e9c52c. Branch sg2000-mainline2 created.
- NEXT: read sample cv1800b-mainline (defconfig, board/milkv-duo/*, git log 4251df2..6f11ffd), then skeleton.

## Key facts (fill in as discovered)
- Sample: configs/rnsbox_milkv_duo_cv1800b_mainline_defconfig, board/milkv-duo/
- Vendor ref (read-only): ~/rnsbox/milkv-duo-01-arm (BSP sipeed), build/boards/sg200x/sg2000_duomodule01_sd/
- host-tools: cp -al from ~/rnsbox/milkv-duo-01-arm/host-tools (do NOT download)
- Heavy builds: RNSBOX_NPROC=4-6; check /mnt/d/Downloads/rnsbox-debug/local-build-rv.log for BUILD-EXIT before big builds
- Target kernel: v7.3-rc7 mainline; dts for sg2000 likely handwritten from vendor dts (cpus, memory, uart0, mmc, clks)
