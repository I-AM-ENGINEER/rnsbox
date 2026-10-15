################################################################################
#
# python-bluezero
#
################################################################################

# Pin to the newest PyPI release (ble-reticulum's requirements.txt floor is
# bluezero>=0.9.1; 0.9.1 is what PyPI serves as latest).
PYTHON_BLUEZERO_VERSION = 0.9.1
PYTHON_BLUEZERO_SOURCE = bluezero-$(PYTHON_BLUEZERO_VERSION).tar.gz
PYTHON_BLUEZERO_SITE = https://files.pythonhosted.org/packages/ca/09/2aec91949397d15acb794648d65646e27f0e9f999a0da962e73023198663
PYTHON_BLUEZERO_SETUP_TYPE = setuptools
PYTHON_BLUEZERO_LICENSE = MIT
PYTHON_BLUEZERO_LICENSE_FILES = LICENSE
# bluezero is a pure-Python wrapper around the BlueZ D-Bus API; it imports
# dbus (dbus-python) at module level in adapter.py/peripheral.py.
PYTHON_BLUEZERO_DEPENDENCIES = python-dbus

$(eval $(python-package))
