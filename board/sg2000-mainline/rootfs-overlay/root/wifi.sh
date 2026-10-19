#!/bin/sh
# AIC8800D80 bring-up for the Duo Module 01 EVB (mainline kernel).
# Modules are deployed to /root by the image build; the LPO/RTC fix and the
# WL_REG_ON power cycle are built into aic8800_bsp (power_on + chip_reset).
exec >/root/wifi.log 2>&1
date
insmod /root/aic8800_bsp.ko sdio_clk_mhz=25
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
