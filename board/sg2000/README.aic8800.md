# AIC8800D80 on the Duo Module 01 EVB (Linux 7.3)

Status: WLAN + BT (hci0 over SDIO) + USB-NCM verified live on 2026-10-20,
kernel 7.3.0-rc7 (`#37`), image built from this board dir.

## Kernel

The only kernel delta against upstream v7.3-rc7 is the board DTS in
`linux-patches/0001-*`: SDHCI1 is the WiFi slot and must use the real card
clock source.

    &sdhci1 {
        clocks = <&clk CLK_SD1>, <&clk CLK_AXI4_SD1>;
        clock-names = "core", "bus";
        assigned-clocks = <&clk CLK_SD1>;
        assigned-clock-parents = <&clk CLK_FPLL>;
        assigned-clock-rates = <375000000>;
        ...
        max-frequency = <25000000>;
        cap-sdio-irq;
        keep-power-in-host;
        non-removable;
    };

Getting `core` wrong (CLK_AXI4_SD1, a gate) makes every requested SDIO clock
land at 75% of nominal; the SDIO high-speed timing then CRC-fails.

**Do not add early-boot debug UART writes or SBI-console markers to the
kernel.** During bring-up the tree carried `uart_mark` writes in head.S and
`RNSBOX_SBI_MARK` busy-wait ecall markers in setup.c/init/main.c. With them
the kernel stopped silently right after `kernel_init()`; removing them made
7.3 boot in one try. Keep such instrumentation out of the shipped series.

## Firmware

Path on the rootfs:
`/usr/lib/firmware/aic8800_sdio/aic8800_and_aic8800D80/`.

Required set (all from the aic8800-sdio-firmware tarball):

    fmacfwbt_8800d80_u02.bin        (WiFi+BT image, SDIO BT transport)
    fw_adid_8800d80_u02.bin
    fw_patch_8800d80_u02.bin
    fw_patch_8800d80_u02_ext0.bin
    fw_patch_table_8800d80_u02.bin

Without the firmware files the bsp fails at `aicbt_patch_table_alloc`, powers
the chip off and the next insmod sees `aicbsp_dummy_sdmmc ... -110`; the
chip_reset sequence below recovers it.

## The 32k LPO fix (the actual "chip app never starts" bug)

The AIC8800 PMU hangs mid-boot if the 32.768 kHz LPO clock is not routed and
the RTC domain is not powered: SDIO registers still answer, the bootrom
protocol works, but firmware upload/start dies with no interrupt ever
arriving (`DBG_START_APP` timeout, flow-control credits 0).

The vendor reference (`build/boards/sg200x/sg2000_wevb_sd/u-boot/cvi_board_init.c`,
`set_rtc_register_for_power()`) writes:

    0x030010B0 = 0x3   PINMUX: CLK32K -> the module's LPO pin
    0x050250AC = 0x2   RTC domain power
    0x050260D0 = 0x3   RTC domain power (0x7 breaks a later boot)

The rv64 board dir has no such u-boot block, so `aic8800_bsp` now performs
these writes in `power_on` (`cvi_lpo_fix()`), immediately followed by the
WL_REG_ON pulse. These registers do not survive a full power cycle, so the
fix must run on every boot - it is idempotent.

## Bring-up sequence

    insmod aic8800_bsp.ko sdio_clk_mhz=25
    echo 4320000.mmc > .../sdhci-dwcmshc/unbind
    echo 1 > /sys/module/aic8800_bsp/parameters/chip_reset   # WL_REG_ON off/on
    sleep 1
    echo 4320000.mmc > .../sdhci-dwcmshc/bind
    sleep 2
    insmod aic8800_fdrv.ko

`chip_reset` also recovers a chip wedged by an earlier failed start. The
`sdio_clk_mhz=25` force is needed because the vendor clock-set block in the
port was compiled out (`#if 0`); the DT caps the bus at 25 MHz anyway.

## BT

BT runs over SDIO function 2 (`CONFIG_SDIO_BT=y`, `fmacfwbt` firmware,
`aic_btsdio` glue) -> `hci0`, not over the module's UART. The BT UART pins
(SoC UART2: K13/K15/L15/K14) are wired on the EVB but unused by the vendor
software; the UART transport is a fallback only.

Modules built by this repo's sg2000 flow:
`aic8800_bsp.ko`, `aic8800_fdrv.ko` (and `aic8800_btlpm.ko`, unloaded).

The external-tree build does this through two packages, selected by
`BR2_PACKAGE_AIC8800` on every AIC board (Duo Module 01, Duo S, LicheeRV
Nano):

  * `package/aic8800` builds the two modules out-of-tree against the
    board's `$(LINUX_DIR)` and installs them to `/root/`;
  * `package/aic8800-sdio-firmware` installs the blob set to
    `/usr/lib/firmware/aic8800_sdio/` (the `CONFIG_AIC_FW_PATH` the bsp
    compiles in).

S35wifi runs `/root/wifi.sh` in the background on every start (skipped
once wlan0 exists, so a portal apply never resets a live chip) and then
waits for wlan0. There is no separate S99 bring-up script any more: the
old one loaded the modules after S35wifi had already given up.

The vendor-BSP flows get the same pair from their own build system
(`build_osdrv` -> `/mnt/system/ko/3rd` plus the `aic8800-sdio-firmware`
Buildroot package).

## Debug log flood (fixed 2026-10-22)

The vendor driver ships with both debug levels wide open — fdrv
`aicwf_dbg_level=1039` (module param), bsp `aicwf_dbg_level_bsp` (was not
even a module param) — and the BT-SDIO glue printed per HCI frame
unconditionally (`AICBT_*` raw printks in `aic_btsdio.h`, plus a per-command
`RNSBOX set_cmd_tx` printk in `rwnx_cmds.c`). At BLE throughput that
flooded syslog fast enough to rotate /var/log/messages every ~30 s (evicting
every other daemon's log) and burned ~40% CPU in the printk/console thread
on the single C906 — enough to starve dropbear.

Fixes, all three needed:
  1. `aic_btsdio.h`: AICBT_DBG/INFO/WARN now gated on fdrv's
     `aicwf_dbg_level` (AICBT_ERR stays unconditional).
  2. `rwnx_cmds.c`: the per-command `RNSBOX set_cmd_tx` printk removed.
  3. `aic_bsp_main.c`: `aicwf_dbg_level_bsp` exposed as a module param.

Runtime quieting (all write 0 to both params, since insmod bypasses
modprobe.d): `root/wifi.sh` right after each insmod, `/etc/init.d/S36aicquiet`
as a boot-time backstop, and `/etc/modprobe.d/aic8800-debug.conf` for any
future modprobe-based load. The LicheeRV Nano (sg2002) overlay
carries the same three files and shares this driver source.
