################################################################################
#
# python-dbus (dbus-python)
#
################################################################################

PYTHON_DBUS_VERSION = 1.5.0
# PyPI normalized the sdist name to underscores (dbus_python-1.5.0.tar.gz).
PYTHON_DBUS_SOURCE = dbus_python-$(PYTHON_DBUS_VERSION).tar.gz
PYTHON_DBUS_SITE = https://files.pythonhosted.org/packages/c1/d3/6be85a9c772d6ebba0cc3ab37390dd6620006dcced758667e0217fb13307
PYTHON_DBUS_SETUP_TYPE = setuptools
PYTHON_DBUS_LICENSE = MIT
PYTHON_DBUS_LICENSE_FILES = COPYING
PYTHON_DBUS_DEPENDENCIES = dbus libglib2 host-pkgconf

# The upstream build system (meson via a setup.py wrapper) cannot
# cross-compile in buildroot: meson would have to run the TARGET python for
# introspection. Our patch adds a plain-setuptools path selected by this env:
# build_ext cross-builds via buildroot's PKG_PYTHON_ENV sysconfig emulation,
# the dbus/glib -I dirs come from staging and dbus-gmain.c is compiled into
# _dbus_glib_bindings (upstream meson links it as a static lib).
PYTHON_DBUS_ENV = \
	DBUS_PYTHON_SIMPLE_BUILD=1 \
	DBUSPY_STAGING=$(STAGING_DIR)

$(eval $(python-package))
