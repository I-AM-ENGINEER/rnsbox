################################################################################
#
# python-rns (Reticulum Network Stack)
#
# The shipped version is the LATEST rns release at build time: rnsbox build.sh
# asks PyPI, rewrites PYTHON_RNS_VERSION (below) + the .hash digest and seeds
# buildroot's dl/ cache with the sha256-verified sdist. This pinned value is
# the offline fallback.
#
################################################################################

PYTHON_RNS_VERSION ?= 1.5.4
PYTHON_RNS_SOURCE = rns-$(PYTHON_RNS_VERSION).tar.gz
PYTHON_RNS_SITE = https://files.pythonhosted.org/packages/7e/fb/d7b0b67c0e369b8533fe14688cb2b65bff21c669b0c776759015ad3d1830
PYTHON_RNS_SETUP_TYPE = pep517
# Install rns as a wheel (dist-info + RECORD) rather than a distutils
# egg-info, so pip can upgrade it in place (portal Update button / manual
# pip install -U rns). rns is setup.py-only, so the PEP 517 build uses the
# setuptools legacy backend -- host-only build deps, no target bloat.
PYTHON_RNS_DEPENDENCIES += host-python-setuptools host-python-wheel
PYTHON_RNS_LICENSE = Reticulum License
PYTHON_RNS_LICENSE_FILES = LICENSE

$(eval $(python-package))
