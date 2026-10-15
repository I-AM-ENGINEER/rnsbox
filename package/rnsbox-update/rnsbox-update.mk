################################################################################
#
# rnsbox-update — A/B whole-OS update agent (python worker + boot hooks)
#
################################################################################

RNSBOX_UPDATE_VERSION = 1.0.0
# Same dual-use source resolution as rnsbox-portal: BR2_EXTERNAL flow and
# the patch-flow BSP buildroot (package rsynced in by build.sh) both work.
RNSBOX_UPDATE_SITE = $(if $(BR2_EXTERNAL_RNSBOX_PATH),$(BR2_EXTERNAL_RNSBOX_PATH),$(TOPDIR))/package/rnsbox-update/src
RNSBOX_UPDATE_SITE_METHOD = local
RNSBOX_UPDATE_LICENSE = MIT
RNSBOX_UPDATE_DEPENDENCIES = python3 xz ca-certificates

define RNSBOX_UPDATE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/rnsbox-sysupdate $(TARGET_DIR)/usr/sbin/rnsbox-sysupdate
	$(INSTALL) -D -m 0755 $(@D)/abselect $(TARGET_DIR)/usr/lib/rnsbox/abselect
	$(INSTALL) -D -m 0755 $(@D)/S90sysupdate $(TARGET_DIR)/etc/init.d/S90sysupdate
	mkdir -p $(TARGET_DIR)/usr/lib/rnsbox/update_providers
	$(INSTALL) -m 0644 $(@D)/update_providers/*.py $(TARGET_DIR)/usr/lib/rnsbox/update_providers/
	# factory default config; on-device /etc/rnsbox/update.conf is never
	# overwritten at runtime (no on-device package manager touches it)
	$(INSTALL) -D -m 0644 $(@D)/update.conf $(TARGET_DIR)/etc/rnsbox/update.conf
endef

$(eval $(generic-package))
