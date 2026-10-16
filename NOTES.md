# aic8800-bt: Bluetooth on Milk-V Duo Module 01(-W) / SG2000 arm64 — work log

Branch: aic8800-bt (in ~/bt-fix, WSL clone of /mnt/c/...GitHub/rnsbox@main f5d6568).
Owner workspace ~/rnsbox (WSL) NOT touched; its warm workdir milkv-duo-01-arm is
copied to ~/bt-fix/milkv-duo-01-arm and all builds run there, serialized by the
shared lock flock ~/rnsbox/.build.lock (deviation from the literal task command
'cd ~/rnsbox && ./build.sh': building there would need my patch commits in the
owner's tree; a private workdir copy keeps both trees safe, same lock).

## Findings (all verified in source)

1. CONFIG_SDIO_BT switches:
   - aic8800_bsp/Makefile:34 CONFIG_SDIO_BT = y (vendor stock, unchanged):
     ccflags -> -DCONFIG_SDIO_BT, so the BSP fw tables compile the fmacfwbt
     variants in. The CURRENTLY SHIPPED aic8800_bsp.ko ALREADY requests
     fmacfwbt.bin (strings: fmacfwbt_8800d80_h_u02.bin present, its
     #else-branch name fmacfw_8800d80x2.bin absent) — the -W radio boots the
     combined WiFi+BT firmware today; only the HCI transport is missing.
   - aic8800_fdrv/Makefile:76 CONFIG_SDIO_BT=n <- RNSBox patch 0011 (licheerv
     series) turned the vendor default y OFF. Gates aic_btsdio.o + btsdio.o
     (Makefile:209-210) and -DCONFIG_SDIO_BT (Makefile:296).
   - top aic8800/Makefile: CONFIG_AIC8800_BTLPM_SUPPORT := n (0011) — set m
     to build aic8800_btlpm.ko (obj- line 5; cp_ext_wireless copies ALL *.ko
     under extdrv/wireless into ko/3rd automatically).
2. Transport choice: aic8800_fdrv/aic_btsdio.h:21
     #ifdef CONFIG_PLATFORM_UBUNTU -> CONFIG_BLUEDROID 0 (BlueZ/kernel HCI)
     #else                        -> CONFIG_BLUEDROID 1 (Android char dev)
   RNSBox exports CONFIG_PLATFORM_UBUNTU=n (0011, CVITEK cross pin) =>
   bluedroid path: /dev/aicbt_dev + a PRIVATE hci_dev — no BlueZ hci0.
   rwnx_main.c:6045: #if CONFIG_BLUEDROID -> btchr_init()+hdev_init();
   #else -> btsdio_init(). btsdio.c is the Marcel Holtmann generic BT SDIO
   driver (kernel <net/bluetooth/hci_core.h>) => REAL hci0 for BlueZ.
   Fix: force '#define CONFIG_BLUEDROID 0' (the Android path is dead code on
   this buildroot image; decoupling from the platform name keeps the CVITEK
   pin intact).
3. Firmware selection (aic8800_bsp/aic_bsp_main.c): fw_u02/fw_u03 tables
   (chip AIC8801, the plain-8800 silicon on the -W board; D80/DC/D80X2 need
   *_8800d80*/*8800dc* fw which slim-target.sh deliberately deletes) pick
   wl_fw = fmacfwbt.bin under CONFIG_SDIO_BT. Path: CONFIG_AIC_FW_PATH
   (/usr/lib/firmware/aic8800_sdio/aic8800_and_aic8800D80, bsp Makefile:5).
   aic8800-sdio-firmware.mk rsyncs the whole tarball there; slim-target keeps
   fmacfwbt.bin + fw_adid/fw_patch/fw_patch_table(.bin, +_u03) — i.e. the BT
   firmware is ALREADY in the image. NO firmware/overlay change needed.
   BT patch loads happen in bsp aicbt_init() before aicwifi_init().
4. btlpm: platform driver 'aic-bt' (self-registering, no DTS node), rfkill
   node starts soft-BLOCKED (rfkill_init_sw_state(true)) and LPM is GPIO/UART
   wake — not needed for BT-over-SDIO. Build+ship it (task), do NOT auto-load
   at boot: it can only soft-block the radio; hci0 comes from fdrv btsdio.
5. Kernel: arm64 defconfig CONFIG_BT=y (builtin; hci_* GPL exports available;
   fdrv is MODULE_LICENSE GPL). RFKILL=y. licheerv-nano/cv1800b kernels keep
   CONFIG_BT off => the driver patch belongs to the milkv-duo-01-arm series
   ONLY (rv64 images stay WiFi-only, unchanged).
6. Userspace: BlueZ + S37bluetooth (hci detect -> /run/bluetooth-hci) and
   rnsd BLE transport already in the image; hci0 appears when aic8800_fdrv
   probes (S35wifi load order: cfg80211, rfkill, bsp, fdrv).

## Patch set (branch aic8800-bt)

- patches/milkv-duo-01-arm/0017-wifi-aic8800-bluetooth-sdio-transport.patch:
  fdrv CONFIG_SDIO_BT=y, BLUEDROID=0, BTLPM_SUPPORT=m.
- board/common/assert-image.sh: cvitek W images assert fmacfwbt.bin + bt
  patch files; aarch64 W images additionally assert the btsdio strings in
  aic8800_fdrv.ko and aic8800_btlpm.ko presence.

## Build/live status

(updated below as work progresses)
