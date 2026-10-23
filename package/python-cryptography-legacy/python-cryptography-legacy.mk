################################################################################
#
# python-cryptography-legacy
#
# 3.3.2 is the last release implemented in C/OpenSSL only (no Rust).  RNS
# accepts it (Provider.py: cryptography >= 2.8) and then routes AES, X25519
# and Ed25519 through OpenSSL's C code instead of RNS's pure-Python fallback.
#
################################################################################

PYTHON_CRYPTOGRAPHY_LEGACY_VERSION = 3.3.2
PYTHON_CRYPTOGRAPHY_LEGACY_SOURCE = cryptography-$(PYTHON_CRYPTOGRAPHY_LEGACY_VERSION).tar.gz
PYTHON_CRYPTOGRAPHY_LEGACY_SITE = https://files.pythonhosted.org/packages/source/c/cryptography
PYTHON_CRYPTOGRAPHY_LEGACY_SETUP_TYPE = setuptools
PYTHON_CRYPTOGRAPHY_LEGACY_LICENSE = Apache-2.0 or BSD-3-Clause
PYTHON_CRYPTOGRAPHY_LEGACY_LICENSE_FILES = LICENSE LICENSE.APACHE LICENSE.BSD
PYTHON_CRYPTOGRAPHY_LEGACY_DEPENDENCIES = host-python-cffi openssl python-cffi python-six
PYTHON_CRYPTOGRAPHY_LEGACY_ENV = OPENSSL_NO_VENDOR=1

$(eval $(python-package))
