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
# The repo is used two ways: as a BR2_EXTERNAL tree (external-flow boards,
# where BR2_EXTERNAL_RNSBOX_PATH is set) and rsynced into a vendor BSP's
# buildroot/package by build.sh (patch-flow boards, where it is not and
# $(TOPDIR) is the buildroot root). Resolve the source dir for both.
RNSBOX_PORTAL_SITE = $(if $(BR2_EXTERNAL_RNSBOX_PATH),$(BR2_EXTERNAL_RNSBOX_PATH),$(TOPDIR))/package/rnsbox-portal/src
RNSBOX_PORTAL_SITE_METHOD = local
RNSBOX_PORTAL_LICENSE = MIT
RNSBOX_PORTAL_LICENSE_FILES = LICENSE
RNSBOX_PORTAL_DEPENDENCIES = openssl zlib

define RNSBOX_PORTAL_BUILD_CMDS
	$(TARGET_CXX) -std=c++17 -Os -s -fno-exceptions -fno-rtti \
		-static-libstdc++ -static-libgcc \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $(@D)/rnsbox-portal $(@D)/cgi/*.cpp -lcrypto -lz
	# rnsbox-mon replaces three BusyBox shell loops on the low-RAM boards
	# (memwatch + memlog + rnstatus warm): one ~200 KB process instead of
	# ~2.2 MB of resident ash.
	$(TARGET_CC) -std=c99 -Os -s \
		-o $(@D)/rnsbox-mon $(@D)/mon/mon.c
endef

define RNSBOX_PORTAL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/rnsbox-portal $(TARGET_DIR)/usr/sbin/rnsbox-portal
	$(INSTALL) -D -m 0755 $(@D)/rnsbox-mon $(TARGET_DIR)/usr/sbin/rnsbox-mon
	mkdir -p $(TARGET_DIR)/www/cgi-bin
	ln -sf /usr/sbin/rnsbox-portal $(TARGET_DIR)/www/cgi-bin/portal
	mkdir -p $(TARGET_DIR)/usr/share/rnsbox-portal/pages
	$(INSTALL) -m 0644 $(@D)/cgi/pages/*.html $(TARGET_DIR)/usr/share/rnsbox-portal/pages/
	$(INSTALL) -D -m 0644 $(@D)/cgi/www/style.css $(TARGET_DIR)/www/style.css
	$(INSTALL) -D -m 0644 $(@D)/cgi/www/index.html $(TARGET_DIR)/www/index.html
	$(INSTALL) -D -m 0644 $(@D)/cgi/www/xterm/xterm.js $(TARGET_DIR)/www/xterm/xterm.js
	$(INSTALL) -D -m 0644 $(@D)/cgi/www/xterm/xterm.css $(TARGET_DIR)/www/xterm/xterm.css
	$(INSTALL) -D -m 0644 $(@D)/cgi/www/xterm/addon-fit.js $(TARGET_DIR)/www/xterm/addon-fit.js
	$(INSTALL) -D -m 0644 $(@D)/cgi/www/xterm/LICENSE $(TARGET_DIR)/www/xterm/LICENSE
	$(INSTALL) -D -m 0644 $(@D)/cgi/www/term.js $(TARGET_DIR)/www/term.js
	mkdir -p $(TARGET_DIR)/www/qr
	$(INSTALL) -m 0644 $(@D)/cgi/www/qr/*.svg $(TARGET_DIR)/www/qr/
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/updatecheck.py $(TARGET_DIR)/usr/lib/rnsbox/updatecheck.py
	# Bundled Reticulum manual: the browser renders the Markdown (marked.js),
	# so page views cost the box nothing and an update only replaces the .md
	# files in /www/manual/docs.
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/manual.py $(TARGET_DIR)/usr/lib/rnsbox/manual.py
	# Map tab: read rnsd's persisted interface-discovery records (coordinates
	# from announces) and a small offline land outline; the browser draws it.
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/mapnodes.py $(TARGET_DIR)/usr/lib/rnsbox/mapnodes.py
	mkdir -p $(TARGET_DIR)/www/map
	$(INSTALL) -m 0644 $(@D)/cgi/www/map/land.geojson $(TARGET_DIR)/www/map/land.geojson
	mkdir -p $(TARGET_DIR)/www/manual/docs $(TARGET_DIR)/www/manual/img
	$(INSTALL) -m 0644 $(@D)/www/manual/marked.min.js $(TARGET_DIR)/www/manual/
	$(INSTALL) -m 0644 $(@D)/docs/manual/*.md $(TARGET_DIR)/www/manual/docs/
	$(INSTALL) -m 0644 $(@D)/docs/VERSION $(TARGET_DIR)/www/manual/docs/VERSION
	$(INSTALL) -m 0644 $(@D)/docs/graphics/*.jpg $(TARGET_DIR)/www/manual/img/
	# Fallback trust store for boards built without ca-certificates: several
	# rnsbox images ship an empty /etc/ssl/certs, so every HTTPS check failed.
	$(INSTALL) -D -m 0644 $(@D)/ca-certificates.crt $(TARGET_DIR)/usr/share/rnsbox-portal/ca-certificates.crt
	# Signed rngit release manifest (trust anchor for Git over Reticulum
	# updates; points at the official Reticulum release node).
	$(INSTALL) -D -m 0644 $(@D)/rns-release.rsm $(TARGET_DIR)/usr/share/rnsbox-portal/rns-release.rsm
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/rnode.py $(TARGET_DIR)/usr/lib/rnsbox/rnode.py
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/rnshaccess.py $(TARGET_DIR)/usr/lib/rnsbox/rnshaccess.py
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/halow.py $(TARGET_DIR)/usr/lib/rnsbox/halow.py
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/rnodebridge $(TARGET_DIR)/usr/lib/rnsbox/rnodebridge
	# S88rnodebridge — ws://:7634 KISS bridge for the embedded RNode flasher;
	# no-op (and no daemon) unless rnode.conf names a transport.
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/S88rnodebridge $(TARGET_DIR)/etc/init.d/S88rnodebridge
	mkdir -p $(TARGET_DIR)/www/flasher
	cp -r $(@D)/cgi/www/flasher/. $(TARGET_DIR)/www/flasher/
	$(INSTALL) -D -m 0755 $(@D)/bin/rnsbox-update-check $(TARGET_DIR)/usr/sbin/rnsbox-update-check
	# S07usbmode — USB role (device|host) from /etc/rnsbox/usb.conf; sorts
	# before the gadget scripts on every board (S08usbdev CVITEK, S10usbdev Pi).
	$(INSTALL) -D -m 0755 $(@D)/cgi/helpers/usbmode $(TARGET_DIR)/etc/init.d/S07usbmode
endef

$(eval $(generic-package))
