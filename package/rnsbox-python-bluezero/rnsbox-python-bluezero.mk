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
