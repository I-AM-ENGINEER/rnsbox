################################################################################
#
# python-nomadnet (Nomad Network — Reticulum LXMF/pages server + TUI)
#
################################################################################

PYTHON_NOMADNET_VERSION = 1.0.4
PYTHON_NOMADNET_SOURCE = nomadnet-$(PYTHON_NOMADNET_VERSION).tar.gz
PYTHON_NOMADNET_SITE = https://files.pythonhosted.org/packages/38/15/f187c64a4e58a89847622869468fa422f14807bbd157d5ff0ce95d5347e6
PYTHON_NOMADNET_SETUP_TYPE = setuptools
PYTHON_NOMADNET_LICENSE = Reticulum License
PYTHON_NOMADNET_LICENSE_FILES = LICENSE

$(eval $(python-package))
