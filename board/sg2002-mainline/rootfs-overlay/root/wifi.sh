#!/bin/sh
# AIC8800D80 bring-up for the LicheeRV Nano (mainline kernel).
# WL_REG_ON = GPIOA26 (vendor u-boot cvi_board_init); GPIOA15 is I2C.
# The DT pinctrl programs the SD1 pads, so skip the EVB pad table.
exec >/root/wifi.log 2>&1
date
insmod /root/aic8800_bsp.ko sdio_clk_mhz=25 wlregon_pin=26 sd1_pads_config=0
echo "bsp rc=$?"
echo 4320000.mmc > /sys/bus/platform/drivers/sdhci-dwcmshc/unbind
echo 1 > /sys/module/aic8800_bsp/parameters/chip_reset
sleep 1
echo 4320000.mmc > /sys/bus/platform/drivers/sdhci-dwcmshc/bind
sleep 2
insmod /root/aic8800_fdrv.ko 2>/dev/null
echo "fdrv rc=$?"
sleep 8
ip link set wlan0 up 2>/dev/null
echo "--- netifs ---"
ls /sys/class/net/
echo WIFI_SCRIPT_DONE
