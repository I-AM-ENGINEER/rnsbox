################################################################################
#
# python-rns-time (time over Reticulum)
#
# Vendored source: the module is tiny, not on PyPI, and carries local changes
# (upstream-NTP server mode, shared-instance operation, --apply for the router
# image) that are not in the upstream repository yet.
#
################################################################################

PYTHON_RNS_TIME_VERSION = 0.2.0
PYTHON_RNS_TIME_SITE = $(BR2_EXTERNAL_RNSBOX_PATH)/package/python-rns-time/src
PYTHON_RNS_TIME_SITE_METHOD = local
PYTHON_RNS_TIME_SETUP_TYPE = setuptools
# No license file in the upstream repository yet; this is the RNSBox delta's
# record, not a claim about the upstream terms.
PYTHON_RNS_TIME_LICENSE = Unknown

$(eval $(python-package))
