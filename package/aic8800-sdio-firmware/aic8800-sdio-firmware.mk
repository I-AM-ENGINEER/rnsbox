################################################################################
#
# aic8800-sdio-firmware — AIC8800 SDIO WiFi/BT firmware blobs
#
################################################################################

# Same pinned tree the CVITEK vendor BSP uses (lxowalle mirror of the
# AIC8800 firmware drop). The blobs are not redistributable source, so the
# license is Proprietary and only the hash pins what we ship.
AIC8800_SDIO_FIRMWARE_VERSION = c56f910044cc854d6c553bcb9a644f3bca5a4c38
AIC8800_SDIO_FIRMWARE_SITE = $(call github,lxowalle,aic8800-sdio-firmware,$(AIC8800_SDIO_FIRMWARE_VERSION))
AIC8800_SDIO_FIRMWARE_LICENSE = Proprietary

define AIC8800_SDIO_FIRMWARE_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/usr/lib/firmware/aic8800_sdio
	cp -a $(@D)/. $(TARGET_DIR)/usr/lib/firmware/aic8800_sdio/
endef

$(eval $(generic-package))
