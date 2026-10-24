################################################################################
#
# aic8800 — AIC8800D80 SDIO WiFi/BT driver (SG2000/SG2002 upstream)
#
################################################################################

AIC8800_VERSION = 1.0
AIC8800_SITE = $(BR2_EXTERNAL_RNSBOX_PATH)/board/sg2000/aic8800
AIC8800_SITE_METHOD = local
AIC8800_LICENSE = GPL-2.0
AIC8800_DEPENDENCIES = linux

# The driver tree is Kbuild-compatible (obj-m on aic8800_bsp/ and
# aic8800_fdrv/); its sub-Makefiles take their platform flags from the
# top-level Makefile, which pins CONFIG_PLATFORM_CVITEK and the firmware
# path. Building through M= keeps those exports and builds exactly the two
# modules /root/wifi.sh insmods.
define AIC8800_BUILD_CMDS
	$(MAKE) $(LINUX_MAKE_FLAGS) -C $(LINUX_DIR) M=$(@D) modules
endef

# Install where /root/wifi.sh loads them: the bsp runs the LPO/RTC and
# WL_REG_ON reset, then the fdrv registers wlan0/hci0.
define AIC8800_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/aic8800_bsp/aic8800_bsp.ko \
		$(TARGET_DIR)/root/aic8800_bsp.ko
	$(INSTALL) -D -m 0644 $(@D)/aic8800_fdrv/aic8800_fdrv.ko \
		$(TARGET_DIR)/root/aic8800_fdrv.ko
endef

$(eval $(generic-package))
