################################################################################
#
# rnsbox-python-bluezero
#
################################################################################

# Same 0.9.1 sdist as the buildroot in-tree python-bluezero package, but
# packaged under its own symbol (see Config.in for why: the in-tree one
# drags in python-gobject / gobject-introspection, which needs glibc).
RNSBOX_PYTHON_BLUEZERO_VERSION = 0.9.1
RNSBOX_PYTHON_BLUEZERO_SOURCE = bluezero-$(RNSBOX_PYTHON_BLUEZERO_VERSION).tar.gz
RNSBOX_PYTHON_BLUEZERO_SITE = https://files.pythonhosted.org/packages/ca/09/2aec91949397d15acb794648d65646e27f0e9f999a0da962e73023198663
RNSBOX_PYTHON_BLUEZERO_SETUP_TYPE = setuptools
RNSBOX_PYTHON_BLUEZERO_LICENSE = MIT
RNSBOX_PYTHON_BLUEZERO_LICENSE_FILES = LICENSE
# bluezero is a pure-Python wrapper around the BlueZ D-Bus API; it imports
# dbus (dbus-python) at module level in adapter.py/peripheral.py.
RNSBOX_PYTHON_BLUEZERO_DEPENDENCIES = dbus-python

$(eval $(python-package))

# GATT-server path: bluezero's async_tools imports gi.repository.GLib, which
# PyGObject cannot provide on the musl rootfs (gobject-introspection needs
# glibc). Ship a small ctypes shim instead - it runs the GLib default main
# context and registers timeouts, which is all bluezero uses; dbus-python's
# own C extension does the actual D-Bus watch integration against the same
# libglib-2.0.
define RNSBOX_PYTHON_BLUEZERO_INSTALL_GI_SHIM
	$(INSTALL) -d $(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR).$(PYTHON3_VERSION_MINOR)/site-packages/gi/repository
	$(INSTALL) -m 0644 $(RNSBOX_PYTHON_BLUEZERO_PKGDIR)/src/gi/__init__.py \
		$(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR).$(PYTHON3_VERSION_MINOR)/site-packages/gi/__init__.py
	$(INSTALL) -m 0644 $(RNSBOX_PYTHON_BLUEZERO_PKGDIR)/src/gi/repository/__init__.py \
		$(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR).$(PYTHON3_VERSION_MINOR)/site-packages/gi/repository/__init__.py
	$(INSTALL) -m 0644 $(RNSBOX_PYTHON_BLUEZERO_PKGDIR)/src/gi/repository/GLib.py \
		$(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR).$(PYTHON3_VERSION_MINOR)/site-packages/gi/repository/GLib.py
endef
RNSBOX_PYTHON_BLUEZERO_POST_INSTALL_TARGET_HOOKS += RNSBOX_PYTHON_BLUEZERO_INSTALL_GI_SHIM
