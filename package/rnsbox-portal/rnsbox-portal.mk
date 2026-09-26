################################################################################
#
# rnsbox-portal — C++/CGI admin UI for the RNSBox router build
#
# A single aarch64 binary (dual-mode):
#   - CGI:  /www/cgi-bin/portal (symlink -> /usr/sbin/rnsbox-portal), executed
#           per-request by uhttpd (S81router). ~0 resident RAM for the web tier.
#           uhttpd follows the symlink (no -S flag) and exec's the extensionless
#           ELF directly; SCRIPT_NAME=/cgi-bin/portal, PATH_INFO=/<route>.
#   - CLI:  `rnsbox-portal nftgen --wan <if> --lan <a,b>` — the nftables
#           generator (single source of truth), invoked by S60routing at boot.
#
# Links the box's OpenSSL (libcrypto) for the pbkdf2 password check; static
# libstdc++/libgcc so only libc + libcrypto are runtime deps. Page shells are
# plain .html under /usr/share/rnsbox-portal/pages; static assets under /www.
#
# Replaces the former Flask/Python portal entirely (no python3 needed for the
# web tier).
#
################################################################################

RNSBOX_PORTAL_VERSION = 0.2.0
RNSBOX_PORTAL_SITE = $(TOPDIR)/package/rnsbox-portal/src
RNSBOX_PORTAL_SITE_METHOD = local
RNSBOX_PORTAL_LICENSE = MIT
RNSBOX_PORTAL_LICENSE_FILES = LICENSE
RNSBOX_PORTAL_DEPENDENCIES = openssl zlib

define RNSBOX_PORTAL_BUILD_CMDS
	$(TARGET_CXX) -std=c++17 -Os -s -fno-exceptions -fno-rtti \
		-static-libstdc++ -static-libgcc \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $(@D)/rnsbox-portal $(@D)/cgi/*.cpp -lcrypto -lz
endef

define RNSBOX_PORTAL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/rnsbox-portal $(TARGET_DIR)/usr/sbin/rnsbox-portal
	mkdir -p $(TARGET_DIR)/www/cgi-bin
	ln -sf /usr/sbin/rnsbox-portal $(TARGET_DIR)/www/cgi-bin/portal
	mkdir -p $(TARGET_DIR)/usr/share/rnsbox-portal/pages
	$(INSTALL) -m 0644 $(@D)/cgi/pages/*.html $(TARGET_DIR)/usr/share/rnsbox-portal/pages/
	$(INSTALL) -D -m 0644 $(@D)/cgi/www/style.css $(TARGET_DIR)/www/style.css
	$(INSTALL) -D -m 0644 $(@D)/cgi/www/index.html $(TARGET_DIR)/www/index.html
	mkdir -p $(TARGET_DIR)/www/qr
	$(INSTALL) -m 0644 $(@D)/cgi/www/qr/*.svg $(TARGET_DIR)/www/qr/
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/updatecheck.py $(TARGET_DIR)/usr/lib/rnsbox/updatecheck.py
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/halow.py $(TARGET_DIR)/usr/lib/rnsbox/halow.py
	$(INSTALL) -D -m 0755 $(@D)/bin/rnsbox-update-check $(TARGET_DIR)/usr/sbin/rnsbox-update-check
endef

$(eval $(generic-package))
