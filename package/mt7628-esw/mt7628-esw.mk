################################################################################
#
# mt7628-esw — minimal MT7628 built-in switch initialisation
#
################################################################################

MT7628_ESW_VERSION = 1.0
MT7628_ESW_SITE = $(BR2_EXTERNAL_RNSBOX_PATH)/package/mt7628-esw/src
MT7628_ESW_SITE_METHOD = local
MT7628_ESW_LICENSE = GPL-2.0
MT7628_ESW_DEPENDENCIES = linux

$(eval $(kernel-module))
$(eval $(generic-package))
