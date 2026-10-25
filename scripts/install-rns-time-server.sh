#!/bin/sh
# install-rns-time-server.sh - install the minimal rns-time server on a host
# that runs (or will run) rnsd. One command, idempotent.
#
#   sudo scripts/install-rns-time-server.sh
#   sudo scripts/install-rns-time-server.sh --rns-config /etc/reticulum \
#        --ntp pool.ntp.org,time.google.com --python /opt/rns-venv/bin/python3
#
# What it does:
#   1. finds the running rnsd's config directory (default /etc/reticulum);
#   2. picks a Python that can `import RNS`, or creates a venv and installs
#      rns when none has it;
#   3. copies the vendored module from this repository to /opt/rns-time;
#   4. installs and starts rnstime-server.service, then prints its
#      destination hash (the value clients must be configured with).
#
# The server does not set the system clock and needs no chrony or GPS: it
# queries the given NTP servers and adds the measured offset to the
# timestamps it serves. It uses the rnsd shared instance, so rnsd must be
# running with share_instance = Yes.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/../package/python-rns-time/src"
RNS_CONFIG=/etc/reticulum
NTP_SERVERS="pool.ntp.org,time.google.com"
PYTHON=
INSTALL_DIR=/opt/rns-time
IDENTITY=/var/lib/rns-time/identity
UNIT=/etc/systemd/system/rnstime-server.service

usage() {
	cat <<'EOF'
usage: install-rns-time-server.sh [options]
  --rns-config DIR   Reticulum config directory (default: from the running
                     rnsd, else /etc/reticulum)
  --ntp LIST         upstream NTP servers, comma separated
  --python PATH      Python interpreter that can import RNS
  --dir PATH         install directory (default /opt/rns-time)
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--rns-config) RNS_CONFIG=${2:?}; RNS_CONFIG_SET=1; shift 2 ;;
		--ntp)         NTP_SERVERS=${2:?}; shift 2 ;;
		--python)      PYTHON=${2:?}; shift 2 ;;
		--dir)         INSTALL_DIR=${2:?}; shift 2 ;;
		-h|--help)     usage; exit 0 ;;
		*)             usage >&2; exit 1 ;;
	esac
done

[ "$(id -u)" -eq 0 ] || { echo "install-rns-time-server: run as root (sudo $0)" >&2; exit 1; }
[ -d "$SRC/rns_time" ] || { echo "install-rns-time-server: $SRC/rns_time not found (run from the rnsbox repository)" >&2; exit 1; }
command -v systemctl >/dev/null 2>&1 || { echo "install-rns-time-server: systemd is required" >&2; exit 1; }

# 1. The running rnsd's --config is the reliable source for the shared
#    instance socket's directory.
if [ -z "${RNS_CONFIG_SET:-}" ]; then
	pid=$(pidof rnsd 2>/dev/null | awk '{print $1}' || true)
	if [ -n "${pid:-}" ] && [ -r "/proc/$pid/cmdline" ]; then
		# shellcheck disable=SC2046
		set -- $(tr '\0' ' ' < "/proc/$pid/cmdline")
		while [ "$#" -gt 0 ]; do
			if [ "$1" = "--config" ] && [ "$#" -ge 2 ]; then
				RNS_CONFIG=$2
				break
			fi
			shift
		done
	fi
fi
[ -r "$RNS_CONFIG/config" ] || echo "install-rns-time-server: warning: $RNS_CONFIG/config is missing; rnsd will create it on first start" >&2
if [ -r "$RNS_CONFIG/config" ] && ! grep -qiE '^[[:space:]]*share_instance[[:space:]]*=[[:space:]]*(yes|true|1)' "$RNS_CONFIG/config"; then
	echo "install-rns-time-server: warning: share_instance is not enabled in $RNS_CONFIG/config; the server needs it" >&2
fi

# 2. A Python with RNS. Prefer --python, then the rnsd venv if its
#    interpreter is visible, then python3 on PATH.
if [ -z "$PYTHON" ]; then
	if [ -n "${pid:-}" ] && [ -r "/proc/$pid/exe" ]; then
		candidate=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
		if [ -n "$candidate" ] && "$candidate" -c 'import RNS' >/dev/null 2>&1; then
			PYTHON=$candidate
		fi
	fi
fi
if [ -z "$PYTHON" ]; then
	for candidate in /opt/rns-venv/bin/python3 /usr/local/bin/python3 /usr/bin/python3 python3; do
		if command -v "$candidate" >/dev/null 2>&1 &&
		   "$candidate" -c 'import RNS' >/dev/null 2>&1; then
			PYTHON=$candidate
			break
		fi
	done
fi
if [ -z "$PYTHON" ]; then
	echo "install-rns-time-server: no Python with RNS found; creating a venv"
	base=$(command -v python3 || true)
	[ -n "$base" ] || { echo "install-rns-time-server: python3 is not installed" >&2; exit 1; }
	"$base" -m venv "$INSTALL_DIR/venv"
	"$INSTALL_DIR/venv/bin/python" -m pip install --upgrade pip >/dev/null
	"$INSTALL_DIR/venv/bin/python" -m pip install rns
	PYTHON=$INSTALL_DIR/venv/bin/python
fi

# 3. Install the module.
mkdir -p "$INSTALL_DIR" /var/lib/rns-time
rm -rf "$INSTALL_DIR/rns_time"
cp -R "$SRC/rns_time" "$INSTALL_DIR/rns_time"
cp "$SRC/setup.py" "$SRC/README.md" "$INSTALL_DIR/" 2>/dev/null || true
chown -R root:root "$INSTALL_DIR" /var/lib/rns-time

# 4. systemd unit + start.
cat > "$UNIT" <<EOF
[Unit]
Description=rns-time: authenticated time over Reticulum
Documentation=https://github.com/wdunn001/rns-time
After=network-online.target rnsd.service
Wants=network-online.target rnsd.service

[Service]
Type=simple
WorkingDirectory=$INSTALL_DIR
Environment=PYTHONPATH=$INSTALL_DIR
ExecStart=$PYTHON -m rns_time.server --config $RNS_CONFIG --identity $IDENTITY --hash-file /run/rns-time.destination --ntp $NTP_SERVERS --interval 300 --announce 1800
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable rnstime-server.service >/dev/null 2>&1 || true
systemctl restart rnstime-server.service
sleep 4

echo
systemctl --no-pager status rnstime-server.service | sed -n '1,4p' || true
echo
if [ -r /run/rns-time.destination ]; then
	echo "rns-time destination: $(cat /run/rns-time.destination)"
	echo "configure clients with that hash (RNSBox: Settings -> Time, or"
	echo "/etc/rnsbox/rnstime.conf servers=<hash>)."
else
	echo "no destination yet; check: journalctl -u rnstime-server -n 30"
fi
