# AIC8800D80 on the Duo Module 01 EVB (mainline 7.3)

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
    0x050260D0 = 0x3   RTC domain power (0x7 breaks a later mainline boot)

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

Modules built by this repo's sg2000-mainline flow:
`aic8800_bsp.ko`, `aic8800_fdrv.ko` (and `aic8800_btlpm.ko`, unloaded).
