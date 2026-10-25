################################################################################
#
# python-ble-reticulum
#
################################################################################

PYTHON_BLE_RETICULUM_VERSION = 0.2.3
# Vendored copy of https://github.com/torlando-tech/ble-reticulum at v0.2.3
# (MIT, see LICENSE). Not on PyPI - the sources ship in this package dir.
# RNSBox deltas:
#   - patches/0001: a Bluetooth-stack probe so the interface degrades into an
#     honest, logged no-op instead of an endless scan-retry loop when the
#     board has no adapter;
#   - src/ble_reticulum/linux_bluetooth_driver.py: a BlueZ Device1.Disconnect
#     on the identity-handshake timeout. A central that connected before it
#     had its transport identity (the Columba Python backend does this on a
#     stale link) never sends the 16-byte RX handshake; without a real LE
#     disconnect it keeps the link alive with keepalives and the handshake is
#     never retried. Dropping the link makes it reconnect and handshake.
#     Same file: central GATT operations address the RX/TX characteristics by
#     the objects resolved from the Reticulum service, not by UUID string —
#     peers whose GATT table registers a characteristic UUID twice (the
#     Columba backend's Reticulum service appears twice) make bleak's
#     collection-wide UUID lookup raise and killed every auto-connect.
#   - src/ble_reticulum/BLEInterface.py: the identity-handshake wait is 10 s
#     instead of 30 (see the comment at _pending_identity_timeout) so the
#     kick-and-reconnect cycle above costs a third of the dead-air time.
PYTHON_BLE_RETICULUM_SOURCE =
PYTHON_BLE_RETICULUM_SITE = $(call qstrip,$(if $(BR2_EXTERNAL_RNSBOX_PATH),$(BR2_EXTERNAL_RNSBOX_PATH),$(TOPDIR))/package/python-ble-reticulum/src)
PYTHON_BLE_RETICULUM_SITE_METHOD = local
PYTHON_BLE_RETICULUM_LICENSE = MIT
PYTHON_BLE_RETICULUM_LICENSE_FILES = LICENSE
PYTHON_BLE_RETICULUM_DEPENDENCIES = python3

# ble_reticulum imports bleak/dbus_fast (central mode) and bluezero/dbus
# (peripheral mode, pairing agent) lazily and guarded - each degrades to a
# feature flag in the code. Declare them all so a BLE-enabled image always
# has the full feature set.
PYTHON_BLE_RETICULUM_DEPENDENCIES += python-bleak rnsbox-python-bluezero dbus-python

# Install layout, matching how RNS loads third-party interfaces (RNS 1.x has
# no built-in BLE type; Reticulum.py exec()s files from <configdir>/interfaces/
# and takes their `interface_class` global):
#
#   site-packages/ble_reticulum/          - the upstream package, importable
#                                           (pip-upgrade-safe: never inside
#                                           the rns dist)
#   /etc/reticulum/interfaces/BLEInterface.py
#                                         - 3-line shim: `from
#                                           ble_reticulum.BLEInterface import
#                                           BLEInterface as interface_class`.
#                                           This is the file rnsd exec()s.
PYTHON_BLE_RETICULUM_INTERFACES_DIR = /etc/reticulum/interfaces
PYTHON_BLE_RETICULUM_SPDIR = /usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages

define PYTHON_BLE_RETICULUM_INSTALL_TARGET_CMDS
	$(INSTALL) -d -m 0755 \
		$(TARGET_DIR)$(PYTHON_BLE_RETICULUM_SPDIR)/ble_reticulum
	for f in __init__.py BLEInterface.py BLEGATTServer.py \
	         BLEFragmentation.py BLEAgent.py bluetooth_driver.py \
	         linux_bluetooth_driver.py RNodeBLE.py; do \
		$(INSTALL) -m 0644 $(@D)/ble_reticulum/$$f \
			$(TARGET_DIR)$(PYTHON_BLE_RETICULUM_SPDIR)/ble_reticulum/$$f || exit 1; \
	done
	$(INSTALL) -d -m 0755 $(TARGET_DIR)$(PYTHON_BLE_RETICULUM_INTERFACES_DIR)
	( \
		echo '# RNSBox shim - the real implementation is the ble_reticulum'; \
		echo '# package in site-packages. RNS exec()s this file from its'; \
		echo '# <configdir>/interfaces/ dir and uses its interface_class.'; \
		echo 'from ble_reticulum.BLEInterface import BLEInterface as interface_class'; \
	) > $(TARGET_DIR)$(PYTHON_BLE_RETICULUM_INTERFACES_DIR)/BLEInterface.py
	chmod 0644 $(TARGET_DIR)$(PYTHON_BLE_RETICULUM_INTERFACES_DIR)/BLEInterface.py
	# Pre-compile bytecode: ~2.5k+2.7k-line modules are slow to parse on the
	# C906 every rnsd start; .pyc cuts that (and target flash reads) down.
	# The host python is the same version as the target python (buildroot
	# builds both from one python3.mk), so the bytecode is ABI-matched.
	PYTHONPATH=$(STAGING_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/ \
	$(HOST_DIR)/bin/python3 -m compileall -q \
		$(TARGET_DIR)$(PYTHON_BLE_RETICULUM_SPDIR)/ble_reticulum \
		|| true
endef

$(eval $(generic-package))
