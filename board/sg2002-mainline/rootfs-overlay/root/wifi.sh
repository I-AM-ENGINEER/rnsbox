#!/bin/sh
# AIC8800D80 bring-up for the LicheeRV Nano (mainline kernel).
# WL_REG_ON = GPIOA26 (vendor u-boot cvi_board_init); GPIOA15 is I2C.
# The DT pinctrl programs the SD1 pads, so skip the EVB pad table.
exec >/root/wifi.log 2>&1
date
insmod /root/aic8800_bsp.ko sdio_clk_mhz=25 wlregon_pin=26 sd1_pads_config=0
echo "bsp rc=$?"
# insmod bypasses modprobe.d, so the debug-silencing options in
# /etc/modprobe.d/aic8800-debug.conf never apply; do it here so every
# (re)load starts quiet - the AIC8800 debug flood is per-SDIO-frame and
# rotates /var/log/messages in under a minute once BLE traffic flows.
echo 0 > /sys/module/aic8800_bsp/parameters/aicwf_dbg_level_bsp 2>/dev/null
echo 4320000.mmc > /sys/bus/platform/drivers/sdhci-dwcmshc/unbind
echo 1 > /sys/module/aic8800_bsp/parameters/chip_reset
sleep 1
echo 4320000.mmc > /sys/bus/platform/drivers/sdhci-dwcmshc/bind
sleep 2
insmod /root/aic8800_fdrv.ko 2>/dev/null
echo "fdrv rc=$?"
echo 0 > /sys/module/aic8800_fdrv/parameters/aicwf_dbg_level 2>/dev/null
sleep 8
ip link set wlan0 up 2>/dev/null
echo "--- netifs ---"
ls /sys/class/net/
echo WIFI_SCRIPT_DONE
