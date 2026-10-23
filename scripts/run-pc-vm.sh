#!/bin/bash
# run-pc-vm.sh — boot the RNSBox pc-x86_64 image in QEMU.
#
#   scripts/run-pc-vm.sh [options] [image]
#
# The image is a plain BIOS disk (grub in the MBR + one ext4 root), so the
# same file boots in QEMU, VirtualBox (BIOS, IDE/SATA) and VMware. QEMU is
# the primary target: this script wires the VM's NICs to mirror the boards'
# network roles:
#
#   eth0  WAN  — QEMU user-net (SLIRP): DHCP -> 10.0.2.15, gateway/DNS via
#                the host stack; the VM has working internet (NTP, PyPI).
#   eth1  LAN  — a second user-net pinned to subnet 10.42.0.0/24. In the
#                guest, S30lan enslaves eth1 into br-lan = 10.42.0.1/24
#                (the role the USB gadget's usb0 plays on real boards), and
#                dnsmasq serves it. Hostfwd rules put the box one TCP hop
#                from the host, exactly like a PC tethered to the board:
#
#                  http://127.0.0.1:8080  -> guest 10.42.0.1:80   (portal)
#                  ssh -p 2222 root@127.0.0.1 -> 10.42.0.1:22     (root/admin)
#                  127.0.0.1:4242 tcp+udp -> 10.42.0.1:4242       (rnsd)
#
# eth1's MAC is pinned, so the bridge/LAN MAC — and rnsd's EUI-64 identity —
# are stable across runs.
#
# By default the VM boots a COPY-ON-WRITE overlay (qcow2 over the raw image)
# in /tmp: the original image file is never modified. Use --persist to boot
# the image in place (or --keep-overlay to inspect the overlay's disk after).
#
# Accel: /dev/kvm is used when present+writable, TCG otherwise (WSL2 usually
# has no /dev/kvm — boots take a few minutes, be patient). Console: the
# serial line (ttyS0), multiplexed with the QEMU monitor on this terminal —
# Ctrl-A X quits, Ctrl-A C switches to the monitor.
#
# Options:
#   -i, --image PATH    image to boot (default: $RNSBOX_PC_IMAGE, then the
#                       build output, then ./rnsbox-pc-x86_64-*.img)
#   -m, --mem MB        guest RAM (default 512)
#       --smp N         guest vCPUs (default 2)
#       --kvm | --tcg   force the accelerator (default: autodetect /dev/kvm)
#       --serial MODE   console mode: stdio (default) | none | pty |
#                       unix:PATH  (applies to ttyS0)
#       --slip-pty      expose ttyS1 as a host pty — the SLIP peer for
#                       S31slip (enable it in the portal/Network or set
#                       /etc/rnsbox/slip.conf enabled=yes; QEMU prints the
#                       /dev/pts/N device it allocated). Talk SLIP from the
#                       host over that pty to emulate the HaLow modem.
#       --nolan         boot without eth1 (WAN-only VM; exercises S30lan's
#                       "LAN without a port" path)
#       --nowan         boot without eth0 (isolated-LAN VM; S30eth just
#                       never gets a lease)
#   -p, --ports A,B,C,D hostfwd ports: portal,ssh,rnsd-tcp,rnsd-udp
#                       (default 8080,2222,4242,4242)
#       --persist       boot the image file itself (writes to it!)
#       --keep-overlay  do not delete the COW overlay on exit
#       --display MODE  QEMU display (default none; e.g. gtk, sdl)
#       --extra ARGS    quoted extra args appended to the QEMU command line
#   -h, --help          this help
set -o pipefail

usage() { sed -n '2,65p' "$0" | sed 's/^# \{0,1\}//'; }

IMAGE="${RNSBOX_PC_IMAGE:-}"
MEM=512
SMP=2
ACCEL=""
SERIAL="stdio"
SLIP_PTY=0
NOLAN=0
NOWAN=0
PORTS="8080 2222 4242 4242"
PERSIST=0
KEEP_OVERLAY=0
DISPLAY="none"
EXTRA=""
while [ $# -gt 0 ]; do
	case "$1" in
		-i|--image)  IMAGE="$2"; shift 2 ;;
		-m|--mem)    MEM="$2"; shift 2 ;;
		--smp)       SMP="$2"; shift 2 ;;
		--kvm)       ACCEL=kvm; shift ;;
		--tcg)       ACCEL=tcg; shift ;;
		--serial)    SERIAL="$2"; shift 2 ;;
		--slip-pty)  SLIP_PTY=1; shift ;;
		--nolan)     NOLAN=1; shift ;;
		--nowan)     NOWAN=1; shift ;;
		-p|--ports)  PORTS="$2"; shift 2 ;;
		--persist)   PERSIST=1; shift ;;
		--keep-overlay) KEEP_OVERLAY=1; shift ;;
		--display)   DISPLAY="$2"; shift 2 ;;
		--extra)     EXTRA="$2"; shift 2 ;;
		-h|--help)   usage; exit 0 ;;
		-*)          echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
		*)           IMAGE="$1"; shift ;;
	esac
done

QEMU="${QEMU:-qemu-system-x86_64}"
command -v "$QEMU" >/dev/null || { echo "QEMU '$QEMU' not found (apt install qemu-system-x86)" >&2; exit 2; }

# ---- locate the image -------------------------------------------------------
if [ -z "$IMAGE" ]; then
	for cand in \
		"$HOME/rnsbox-work/upstream/out/pc-x86_64/images/rnsbox-pc-x86_64.img" \
		./rnsbox-pc-x86_64-*.img; do
		[ -f "$cand" ] && { IMAGE="$cand"; break; }
	done
fi
[ -n "$IMAGE" ] && [ -f "$IMAGE" ] || { echo "no image found — pass one: $0 -i IMAGE" >&2; exit 2; }
IMAGE="$(cd "$(dirname "$IMAGE")" && pwd)/$(basename "$IMAGE")"

# ---- accelerator ------------------------------------------------------------
if [ -z "$ACCEL" ]; then
	if [ -e /dev/kvm ] && [ -w /dev/kvm ]; then
		ACCEL=kvm
		echo ">> /dev/kvm found: KVM acceleration"
	else
		ACCEL=tcg
		echo ">> no /dev/kvm: TCG emulation (slower; be patient)"
	fi
fi

# ---- disk: in-place, or a COW overlay so the image stays pristine -----------
OVERLAY=""
if [ "$PERSIST" = 1 ]; then
	DRIVE=(-drive "file=$IMAGE,format=raw,if=ide")
	echo ">> booting the image IN PLACE: $IMAGE"
else
	OVERLAY="$(mktemp -t rnsbox-pc-disk.XXXXXX.qcow2)"
	qemu-img create -q -f qcow2 -b "$IMAGE" -F raw "$OVERLAY" || exit 2
	DRIVE=(-drive "file=$OVERLAY,format=qcow2,if=ide")
	echo ">> booting a COW overlay over: $IMAGE"
fi
cleanup() {
	[ -n "$OVERLAY" ] && [ -e "$OVERLAY" ] && [ "$KEEP_OVERLAY" = 0 ] && rm -f "$OVERLAY"
}
trap cleanup EXIT

# ---- NICs -------------------------------------------------------------------
NETDEV=()
if [ "$NOWAN" = 0 ]; then
	NETDEV+=(-netdev user,id=n0 -device virtio-net-pci,netdev=n0,mac=52:54:00:52:53:42)
fi
if [ "$NOLAN" = 0 ]; then
	set -- $PORTS
	P_WEB=$1; P_SSH=$2; P_RNS=$3; P_RNSU=$4
	FWD="hostfwd=tcp:127.0.0.1:${P_WEB}-10.42.0.1:80,hostfwd=tcp:127.0.0.1:${P_SSH}-10.42.0.1:22,hostfwd=tcp:127.0.0.1:${P_RNS}-10.42.0.1:4242,hostfwd=udp:127.0.0.1:${P_RNSU}-10.42.0.1:4242"
	NETDEV+=("-netdev" "user,id=n1,net=10.42.0.0/24,dhcpstart=10.42.0.100,${FWD}" \
	         "-device" "virtio-net-pci,netdev=n1,mac=52:54:00:52:53:43")
	echo ">> hostfwd: http://127.0.0.1:${P_WEB}  ssh -p ${P_SSH} root@127.0.0.1  rns 127.0.0.1:${P_RNS} (tcp/udp)"
fi

# ---- serial -----------------------------------------------------------------
SERIAL_ARGS=(-display "$DISPLAY" -monitor none)
case "$SERIAL" in
	stdio)    SERIAL_ARGS+=(-serial mon:stdio) ;;
	none)     SERIAL_ARGS+=(-serial none) ;;
	unix:*)   SERIAL_ARGS+=(-serial "$SERIAL,server=on,wait=off") ;;
	pty)      SERIAL_ARGS+=(-serial pty) ;;
	*)        echo "bad --serial mode: $SERIAL" >&2; exit 2 ;;
esac
if [ "$SLIP_PTY" = 1 ]; then
	SERIAL_ARGS+=(-serial pty)
	echo ">> SLIP port: ttyS1 is a host pty (QEMU prints the /dev/pts/N on stderr)"
fi

echo ">> starting QEMU ($ACCEL, ${MEM}MB, smp=$SMP): $IMAGE"
if [ "$DISPLAY" = none ]; then
	echo ">> console: serial (Ctrl-A X quits, Ctrl-A C = QEMU monitor)"
fi

"$QEMU" \
	-machine pc -m "$MEM" -smp "$SMP" \
	-accel "$ACCEL" \
	"${DRIVE[@]}" \
	"${NETDEV[@]}" \
	"${SERIAL_ARGS[@]}" \
	-rtc base=utc \
	$EXTRA
rc=$?
[ $rc -ne 0 ] && echo ">> QEMU exited with status $rc"
exit $rc
