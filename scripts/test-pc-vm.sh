#!/bin/bash
# test-pc-vm.sh — smoke/acceptance test for the RNSBox pc-x86_64 image.
#
#   scripts/test-pc-vm.sh [options] [image]
#
# Boots the image in QEMU (a throwaway COW overlay — the image file is never
# modified), waits for boot over the serial console, logs in as root and
# exercises the full router stack the way a board bring-up would:
#
#   boot       kernel is 6.12.x-rnsbox, no panic, login prompt on ttyS0
#   network    br-lan exists with 10.42.0.1/24 (/run/lan-iface = br-lan),
#              eth0 got a DHCP lease from QEMU user-net (S30eth works)
#   firewall   nftables ruleset loaded (drop policy in place)
#   services   dnsmasq, uhttpd (portal), rnsd all running; rnsd listens :4242
#   portal     GET / serves the docroot, GET /login serves the login page,
#              POST admin/admin logs in (302 + session cookie), the
#              dashboard + network + reticulum tabs render with the cookie
#
# Prints one [ ok  ] / [ FAIL ] line per check and a final summary; exits 0
# only if every check passed. This is the prototype of the CI acceptance
# run: the same harness gates real-board releases later (rpi0-2w first).
#
# Options:
#   -i, --image PATH   image to test (default: $RNSBOX_PC_IMAGE, then the
#                      build output, then ./rnsbox-pc-x86_64-*.img)
#   -m, --mem MB       guest RAM (default 512)
#       --kvm | --tcg  force the accelerator (default: autodetect /dev/kvm)
#   -t, --timeout S    boot timeout in seconds (default 420 — TCG is slow)
#       --keep         keep the VM running after the test (prints the
#                      hostfwd ports and the console socket path)
#   -h, --help         this help
set -o pipefail

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

IMAGE="${RNSBOX_PC_IMAGE:-}"
MEM=512
ACCEL=""
BOOT_TIMEOUT=420
KEEP=0
while [ $# -gt 0 ]; do
	case "$1" in
		-i|--image) IMAGE="$2"; shift 2 ;;
		-m|--mem)   MEM="$2"; shift 2 ;;
		--kvm)      ACCEL=kvm; shift ;;
		--tcg)      ACCEL=tcg; shift ;;
		-t|--timeout) BOOT_TIMEOUT="$2"; shift 2 ;;
		--keep)     KEEP=1; shift ;;
		-h|--help)  usage; exit 0 ;;
		-*)         echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
		*)          IMAGE="$1"; shift ;;
	esac
done

QEMU="${QEMU:-qemu-system-x86_64}"
command -v "$QEMU" >/dev/null || { echo "QEMU not found (apt install qemu-system-x86)" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 2; }
command -v curl >/dev/null   || { echo "curl not found" >&2; exit 2; }

if [ -z "$IMAGE" ]; then
	for cand in \
		"$HOME/rnsbox-work/upstream/out/pc-x86_64/images/rnsbox-pc-x86_64-lite.img" \
		./rnsbox-pc-x86_64-*.img; do
		[ -f "$cand" ] && { IMAGE="$cand"; break; }
	done
fi
[ -n "$IMAGE" ] && [ -f "$IMAGE" ] || { echo "no image found — pass one: $0 -i IMAGE" >&2; exit 2; }
IMAGE="$(cd "$(dirname "$IMAGE")" && pwd)/$(basename "$IMAGE")"

if [ -z "$ACCEL" ]; then
	if [ -e /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL=kvm; else ACCEL=tcg; fi
fi

# pick free host ports for the hostfwd rules (fixed ports would collide with
# anything else running on this box)
read -r P_WEB P_SSH P_RNS P_RNSU <<< "$(python3 - <<'PY'
import socket
ps = []
for typ in (socket.SOCK_STREAM, socket.SOCK_STREAM, socket.SOCK_STREAM, socket.SOCK_DGRAM):
    s = socket.socket(socket.AF_INET, typ)
    s.bind(("127.0.0.1", 0))
    ps.append(s.getsockname()[1])
    s.close()
print(*ps)
PY
)"

WORK="$(mktemp -d /tmp/rnsbox-pc-smoke.XXXXXX)"
LOG="$WORK/smoke.log"
: > "$LOG"
FAIL=0

qemu-img create -q -f qcow2 -b "$IMAGE" -F raw "$WORK/disk.qcow2" || exit 2

CONSOLE="$WORK/console.sock"
echo ">> smoke test: $IMAGE"
echo ">> accel=$ACCEL  workdir=$WORK  hostfwd: web=$P_WEB ssh=$P_SSH rns=$P_RNS/$P_RNSU"

"$QEMU" \
	-machine pc -m "$MEM" -smp 2 \
	-accel "$ACCEL" \
	-drive "file=$WORK/disk.qcow2,format=qcow2,if=ide" \
	-netdev user,id=n0 \
	-device virtio-net-pci,netdev=n0,mac=52:54:00:52:53:42 \
	-netdev "user,id=n1,net=10.42.0.0/24,dhcpstart=10.42.0.100,hostfwd=tcp:127.0.0.1:${P_WEB}-10.42.0.1:80,hostfwd=tcp:127.0.0.1:${P_SSH}-10.42.0.1:22,hostfwd=tcp:127.0.0.1:${P_RNS}-10.42.0.1:4242,hostfwd=udp:127.0.0.1:${P_RNSU}-10.42.0.1:4242" \
	-device virtio-net-pci,netdev=n1,mac=52:54:00:52:53:43 \
	-display none -monitor none \
	-serial "unix:$CONSOLE,server=on,wait=off" \
	-rtc base=utc \
	>"$WORK/qemu.stdout" 2>"$WORK/qemu.stderr" &
QPID=$!
trap '[ -n "$KEEP" ] || kill "$QPID" 2>/dev/null; [ -n "$KEEP" ] || rm -rf "$WORK"' EXIT

# ---- the whole guest conversation lives in python (serial socket expect) ----
python3 - "$CONSOLE" "$BOOT_TIMEOUT" "$P_WEB" 2>"$WORK/probe.err" | tee "$WORK/probe.out" <<'PYEOF'
import re, socket, sys, time, http.client, urllib.parse

console, timeout, web_port = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
buf = b""
sock = None
t_end = time.time() + timeout
n_fail = 0

def record(name, ok, detail=""):
    global n_fail
    ok = bool(ok)
    n_fail += 0 if ok else 1
    print(("[ ok  ] " if ok else "[ FAIL] ") + name + ((" — " + detail) if detail else ""), flush=True)

def connect():
    global sock
    while time.time() < t_end:
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(2)
            sock.connect(console)
            return True
        except (FileNotFoundError, ConnectionRefusedError, OSError):
            time.sleep(0.5)
    return False

def pump(drain=False):
    global buf
    try:
        sock.settimeout(0.1 if drain else 2)
        while True:
            d = sock.recv(65536)
            if not d:
                break
            buf += d
    except socket.timeout:
        pass

def wait_for(pattern, timeout_s):
    global buf
    t0 = time.time()
    rx = re.compile(pattern.encode())
    while time.time() - t0 < timeout_s and time.time() < t_end:
        pump()
        m = rx.search(buf)
        if m:
            out = buf[:m.end()]
            buf = buf[m.end():]
            return out.decode("utf-8", "replace")
    return None

def run_cmd(cmd, timeout_s=60):
    """Run one shell command on the guest; return its output (marker-stripped)."""
    global buf
    buf = b""
    marker = "___DONE_%d___" % run_cmd.n
    run_cmd.n += 1
    sock.sendall(cmd.encode() + b"; echo " + marker.encode() + b"\n")
    out = wait_for(marker, timeout_s)
    if out is None:
        return None
    lines = out.split("\r\n")
    # drop the echoed command line itself
    if lines and cmd.split(";")[0].strip()[:16] in lines[0]:
        lines = lines[1:]
    return "\n".join(lines)
run_cmd.n = 0

def http_req(method, path, body=None, cookie=None):
    conn = http.client.HTTPConnection("127.0.0.1", web_port, timeout=30)
    hdrs = {"Cookie": cookie} if cookie else {}
    conn.request(method, path, body=body, headers=hdrs)
    r = conn.getresponse()
    data = r.read()
    setc = r.getheader("Set-Cookie") or ""
    conn.close()
    return r.status, data, setc

# ---- boot -------------------------------------------------------------------
if not connect():
    record("boot: console reachable", False, "socket never appeared")
    sys.exit(1)
banner = wait_for(r"rnsbox login:", timeout)
record("boot: login prompt on serial (ttyS0)", banner is not None,
       "" if banner else "no 'rnsbox login:' within %.0fs" % timeout)
if banner is None:
    sys.exit(1)
record("boot: no kernel panic in console log", "Kernel panic" not in banner)

# ---- login ------------------------------------------------------------------
sock.sendall(b"root\n")
wait_for(r"Password:", 30)
sock.sendall(b"admin\n")
pr = wait_for(r"root@rnsbox:[^\r\n]*#", 30)
record("login: root/admin over serial", pr is not None)

# ---- in-guest checks --------------------------------------------------------
u = run_cmd("uname -r")
record("boot: kernel is 6.12.x-rnsbox",
       u is not None and "6.12." in u and "rnsbox" in u, (u or "no answer").strip())

li = run_cmd("cat /run/lan-iface")
record("lan: /run/lan-iface = br-lan", li is not None and "br-lan" in li, (li or "").strip())

ip = run_cmd("ip -4 addr show br-lan")
record("lan: br-lan has 10.42.0.1/24", ip is not None and "10.42.0.1/24" in ip)

v6 = run_cmd("sysctl -n net.ipv6.conf.br-lan.disable_ipv6")
record("lan: IPv6 enabled on br-lan (RNS AutoInterface)",
       v6 is not None and v6.strip().endswith("0"))

wan = run_cmd("ip -4 addr show eth0")
record("wan: eth0 got a DHCP lease (QEMU user-net)", wan is not None and "inet 10.0.2." in wan)

dnsmasq = run_cmd("pidof dnsmasq")
record("service: dnsmasq running", dnsmasq is not None and dnsmasq.strip() != "")

uhttpd = run_cmd("pidof uhttpd")
record("service: uhttpd (portal web tier) running", uhttpd is not None and uhttpd.strip() != "")

rnsd = run_cmd("pidof rnsd")
record("service: rnsd running", rnsd is not None and rnsd.strip() != "")

port = run_cmd("netstat -tln 2>/dev/null | grep -c ':4242 '")
record("service: rnsd listens on :4242",
       port is not None and port.strip().lstrip("0 ") != "")

nft = run_cmd("nft list ruleset 2>/dev/null | grep -c 'policy drop'")
record("firewall: nftables ruleset loaded (drop policy)",
       nft is not None and nft.strip() not in ("", "0"))

slip = run_cmd("grep -c 'enabled=no' /etc/rnsbox/slip.conf")
record("slip: ships disabled (ttyS1 free for the pty modem peer)",
       slip is not None and slip.strip() not in ("", "0"))

# ---- portal over hostfwd ----------------------------------------------------
try:
    st, data, _ = http_req("GET", "/")
    record("portal: GET / serves the docroot (uhttpd)",
           st == 200 and (b"RNSBox" in data or b"<html" in data.lower()), "status %d" % st)

    st, data, _ = http_req("GET", "/cgi-bin/portal/login")
    record("portal: GET /login serves the login page",
           st == 200 and b"Sign in" in data, "status %d" % st)

    body = urllib.parse.urlencode({"username": "admin", "password": "admin", "next": ""})
    st, data, setc = http_req("POST", "/cgi-bin/portal/login", body.encode())
    cookie = setc.split(";")[0] if "rnsbox_session=" in setc else ""
    record("portal: POST login admin/admin -> 302 + session cookie",
           st in (302, 303) and cookie.startswith("rnsbox_session="), "status %d" % st)

    st, data, _ = http_req("GET", "/cgi-bin/portal/", cookie=cookie)
    record("portal: dashboard renders with the session",
           st == 200 and (b"Dashboard" in data or b"RNSBox" in data), "status %d" % st)

    st, data, _ = http_req("GET", "/cgi-bin/portal/network", cookie=cookie)
    record("portal: network tab renders", st == 200 and b"Network" in data, "status %d" % st)

    st, data, _ = http_req("GET", "/cgi-bin/portal/reticulum", cookie=cookie)
    record("portal: reticulum tab renders", st == 200 and b"Reticulum" in data, "status %d" % st)
except Exception as e:
    record("portal: HTTP checks", False, "exception: %r" % e)

# ---- graceful shutdown ------------------------------------------------------
try:
    sock.sendall(b"poweroff\n")
except OSError:
    pass

sys.exit(1 if (n_fail or False) else 0)
PYEOF
rc=${PIPESTATUS[0]}

# ---- wait for qemu to exit (the probe sent poweroff) ------------------------
if [ "$rc" = 0 ] && [ -z "$KEEP" ]; then
	for _ in $(seq 1 60); do
		kill -0 "$QPID" 2>/dev/null || break
		sleep 1
	done
	kill -0 "$QPID" 2>/dev/null && {
		echo ">> VM did not power off in 60s; killing" | tee -a "$LOG"
		kill "$QPID" 2>/dev/null
	}
else
	# probe failed early — stop the VM, keep the workdir for debugging
	sleep 2
	kill "$QPID" 2>/dev/null
fi

if [ "$rc" != 0 ]; then
	echo "[ FAIL] probe crashed or a check failed" | tee -a "$LOG"
	echo ">> probe stderr: $WORK/probe.err   console/qemu logs in: $WORK" | tee -a "$LOG"
	FAIL=$((FAIL+1))
	trap - EXIT   # keep the debugging artifacts
fi

PASS=0; EXTRA_FAIL=0
if [ -f "$WORK/probe.out" ]; then
	PASS="$(grep -c '^\[ ok  \]' "$WORK/probe.out")"
	EXTRA_FAIL="$(grep -c '^\[ FAIL\]' "$WORK/probe.out")"
fi
FAIL=$(( FAIL + EXTRA_FAIL ))

echo "" | tee -a "$LOG"
echo "=================== SMOKE RESULT: $([ "$FAIL" = 0 ] && echo PASS || echo FAIL) (pass=$PASS fail=$FAIL) ===================" | tee -a "$LOG"

if [ -n "$KEEP" ]; then
	echo ">> keeping VM running (pid $QPID):"
	echo ">>   portal http://127.0.0.1:$P_WEB   ssh -p $P_SSH root@127.0.0.1   console: $CONSOLE"
	echo ">>   qemu stderr: $WORK/qemu.stderr"
	trap - EXIT   # do not clean the workdir or kill the VM
fi
[ "$FAIL" = 0 ]
