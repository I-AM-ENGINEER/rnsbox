################################################################################
#
# ht-hd01-firmware — vendor blobs for the Heltec HT-HD01-V2
#
################################################################################

HT_HD01_FIRMWARE_VERSION = 1.0
HT_HD01_FIRMWARE_SITE = $(BR2_EXTERNAL_RNSBOX_PATH)/package/ht-hd01-firmware/files
HT_HD01_FIRMWARE_SITE_METHOD = local
HT_HD01_FIRMWARE_LICENSE = Proprietary
HT_HD01_FIRMWARE_REDISTRIBUTE = NO

define HT_HD01_FIRMWARE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/mm6108.bin \
		$(TARGET_DIR)/lib/firmware/morse/mm6108.bin
	$(INSTALL) -D -m 0644 $(@D)/bcf_HD01_v2.bin \
		$(TARGET_DIR)/lib/firmware/morse/bcf_HD01_v2.bin
	$(INSTALL) -D -m 0644 $(@D)/mt7628_e1.bin \
		$(TARGET_DIR)/lib/firmware/mt7628_e1.bin
	$(INSTALL) -D -m 0644 $(@D)/mt7628_e2.bin \
		$(TARGET_DIR)/lib/firmware/mt7628_e2.bin
endef

$(eval $(generic-package))
