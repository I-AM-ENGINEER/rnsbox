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

## Build (DONE)

- Standalone module build OK (aarch64, vermagic 5.10.4-rnsbox): fdrv with
  btsdio (btsdio_init strings present, bluedroid /dev/aicbt_dev compiled
  out), bsp unchanged-behaviour rebuild, btlpm 34 KB. fdrv grew 744->797 KB
  unstripped.
- Full image: flock ~/rnsbox/.build.lock; cd ~/bt-fix && RNSBOX_NPROC=8
  ./build.sh sg2000-arm64 lite -> BUILD-EXIT=0 (image
  2026-10-17-02-50-00b767-lite.img). Note: copied workdir needed
  'rm -rf freertos/cvitek/build' once — the warm CMakeCache.txt embeds
  absolute paths of the source workspace.
- assert-image.sh cvitek: 0 failures, incl. the new BT asserts
  (fmacfwbt.bin/fw_adid/fw_patch/fw_patch_table shipped; btsdio transport
  in fdrv; btlpm.ko present).
- Image copied to ~/bt-fix-artifacts/rnsbox-sg2000-arm64-aic8800bt-00b767-lite.img
  (owner will place it on D:).

## LIVE on the flashed board (2026-10-17, image 01:21 c778e3, merged main)

Board: admin@192.168.88.230 works (password rnsbox-test1). Findings:

1. ROOT: sudo NOT present; busybox carries the setuid bit but drops
   privileges (FEATURE_SUID: '/usr/bin/busybox id' -> uid=103); /bin/su is
   there but root is locked. => ko insmod / /mnt/system/ko/3rd writes are
   impossible over SSH as admin. Escalated to the owner (serial-console
   root per the 0012 policy is the natural path).
2. RADIO IS NOT ANSWERING (independent of my BT work):
   - mmc1 SDHCI 4320000.wifi-sd up, /sys/bus/mmc/devices = mmc0:aaaa only
     (SD card), /sys/bus/sdio/devices EMPTY — no SDIO function on the bus;
   - dmesg: aic8800_bsp loads, then 'aicbsp_set_subsys AIC_WIFI fail to
     set power state to 1' twice, 'rwnx_mod_init, set power on fail!' —
     the stock WiFi-only fdrv already fails the same way;
   - /var/log/wifi.log: 'no wlan0 after module load — non-W board or
     driver fault; exiting'; /run/bluetooth-hci = 'no adapter';
   - cvi_wifi_pin driver bound, wifi-sd dtb node okay — sequencing is in
     place, the chip never shows up on CMD5.
   => Either the module is radio-less (not a -W) or the radio HW path is
   dead. SOFT-FAILURE VERIFIED: board fully usable (eth0/usb0/portal/ssh),
   S35wifi/S37bluetooth exit 0, bluetoothd runs, no boot delays/crashes.
3. BlueZ userspace on the image confirmed: bluetoothctl, hciconfig,
   /usr/libexec/bluetooth/bluetoothd -n -E running; S40bluetoothd present.

Conclusion: the software path is complete and shipped in the branch (driver
patch + firmware already in every -W image + asserts + image artifact).
The live hci0 proof is gated on (a) root for insmod and (b) a radio that
answers on the SDIO bus; both escalated to the owner.

## Delivery decision (coordinator, 2026-10-17)

- Live insmod tests on the currently flashed board are CANCELLED: no root on
  the board, and the SDIO bus is empty anyway (radio absent or dead — owner
  asked whether the module is a -W at all).
- The DELIVERABLE is this branch's image:
  ~/bt-fix-artifacts/rnsbox-sg2000-arm64-aic8800bt-00b767-lite.img
  (owner copies to D:\Downloads as rnsbox-sg2000-arm64-aic8800bt*.img and
  flashes). It carries the BT-capable kos + btlpm + fmacfwbt firmware and
  passes assert-image 0/0.
- After the owner flashes it: verify via portal http://10.42.0.1
  (admin/admin over the USB network) -> POST settings/ssh -> ssh
  admin@192.168.88.230 -> dmesg | grep -iE 'aic|btsdio' -> ls
  /sys/class/bluetooth -> bluetoothctl show. If the module is genuinely
  radio-less, everything stays a logged no-op (verified above).

## LIVE BREAKTHROUGH (2026-10-17, root via u-boot init=/bin/sh)

The radio is an AIC8800D80 (SDIO vid 0xc8a1, did 0x0082, func2 0x0182,
chip rev bits 7) — NOT the plain 8800 the slim rule assumed. Root
debugging (devmem + a diagnostic sdio driver dumping func1 regs) found
why the chip was silent:

1. WL_REG_ON (XGPIOA_15, pad 'SPK_EN' @0x0300103C) was never driven:
   pad mux was already GPIO(3), but GPIOA DIR bit15 = input. Driving it
   high + cvi_sdio_rescan() (exported by sdhci-cv181x, no in-tree
   caller; triggered via a 10-line rescan.ko) enumerates the card:
   'mmc1: new high speed SDIO card at address 390b'.
2. With the card up, the bsp still wedged: first 512-byte message stuck
   at flow-ctrl (Q1 reg 0x03 = 0 forever) -> cmd 1024/1025 timeout.
   Raw register dump (sdiodiag.ko): INTR_PENDING=0x10, FLOW_CTRL_Q1=0.
   Root cause: the 32.768 kHz LPO clock is not routed to the module —
   the chip PMU hangs mid-boot (SDIO regs alive, ring dead). The
   working DuoS u-boot does PINMUX_CONFIG(CLK32K, PWR_GPIO_10) i.e.
   pad 0x030010B0 = 3, plus RTC-domain power 0x050250AC=0x2 and
   0x050260D0=0x7 (wevb reference: set_rtc_register_for_power()).
3. After devmem(0x050260D0=0x7, 0x050250AC=0x2, CLK32K mux) + fresh
   WL_REG_ON pulse + rescan: bsp + BT-fdrv load CLEAN, firmware
   fmacfwbt_8800d80_u02.bin boots, and:

   wlan0: UP, MAC 88:00:33:77:a6:c7 — iw scan sees real networks
          (MikroTik-29E940, LG A/C, Gorod-118...);
   hci0:  Type Primary Bus SDIO, BD 0B:3B:22:AC:88:20, UP RUNNING,
          RX 47k / 1708 events / 0 errors; bluetoothctl: Controller
          0B:3B:22:AC:88:20 (public), Powered: yes (BlueZ 5.68).

The btsdio transport from 0017 works unchanged (aic_btsdio skb type 4
events flowing; hci0 registered by fdrv probe).

## Fixes baked into the branch

- patches/milkv-duo-01-arm/0018: u-boot cvi_board_init.c for
  sg2000_duo01evb_arm64_sd — RTC power, CLK32K LPO mux, SDIO1 pads,
  WL_REG_ON GPIO drive with reset pulse. (Kernel boot scan then finds
  the card; no rescan.ko needed at boot.)
- board/common/slim-target.sh: KEEP the *_8800d80* firmware (the old
  rule deleted the exact set this radio boots).
- board/common/assert-image.sh: assert the five D80 firmware files.

Debug helpers (NOT in the tree, kept in ~/bt-fix/{rescan-mod,sdiodiag}):
rescan.ko calls cvi_sdio_rescan(); sdiodiag.ko dumps func1 regs 0x00-0x12
+ flow-ctrl polling. The D80 firmware for live tests came from the
aic8800-sdio-firmware tarball already in buildroot/dl.

Open items (documented, not blocking -W arm64):
- rv64 board dirs (sg2000_duo_sd, licheervnano) lack the same u-boot
  wifi block — their -W WiFi/BT stays dead until the same lines are
  ported (different series).
- sg2002_duo_arm64_sd (QFN carrier) likely needs the SG2002 pad
  equivalents; untouched.
- UART2->UART4 BT HCI mux (DuoS) not needed: BT runs over SDIO here;
  btlpm ships but stays unloaded (rfkill would soft-block).
- /run/bluetooth-hci still says 'no adapter' until a re-boot with the
  fixed u-boot (S37bluetooth runs before the radio exists on the old
  image; rnsd re-checks every 30 s and will pick hci0 up live).
