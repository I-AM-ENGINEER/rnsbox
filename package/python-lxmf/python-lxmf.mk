################################################################################
#
# python-lxmf (Lightweight eXtensible Message Format for Reticulum)
#
# NOTE: PyPI dropped sdists for lxmf at 0.9.5+ — only wheels published.
# We pull the GitHub release-tag tarball instead (same content). Using
# the github buildroot macro so the URL composes correctly and the
# downloaded file gets saved as python-lxmf-0.9.8.tar.gz.
#
################################################################################

PYTHON_LXMF_VERSION = 0.9.8
PYTHON_LXMF_SITE = $(call github,markqvist,LXMF,$(PYTHON_LXMF_VERSION))
PYTHON_LXMF_SETUP_TYPE = setuptools
PYTHON_LXMF_LICENSE = Reticulum License
PYTHON_LXMF_LICENSE_FILES = LICENSE

$(eval $(python-package))
