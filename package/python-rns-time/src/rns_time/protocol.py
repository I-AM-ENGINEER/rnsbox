"""rns-time protocol v1: wire format + offset math (shared by server and client)."""
import time

try:
    import umsgpack  # standalone package (pip install umsgpack)
except ImportError:  # RNS pip package vendors it, but not top-level
    from RNS.vendor import umsgpack

VERSION = 1
APP_NAME = "rnstime"
ASPECTS = ("time",)

# MeshAPI (github.com/wdunn001/meshapi): a request-handler path alongside the
# packet-based NTP exchange, so MeshAPI clients can discover this service and
# call a simple request/response "now" op. The precise 4-timestamp sync stays on
# the packet path above.
MESHAPI_PATH = "t"
MANIFEST_OP = "__manifest__"

# Server-side health: refuse to advertise healthy time above this stratum.
# Clients ignore servers at or above it (holdover / unsynchronised).
MAX_HEALTHY_STRATUM = 9


def pack(obj):
    return umsgpack.packb(obj)


def pack_request(t1=None):
    """Client -> server. Returns (payload_bytes, t1)."""
    if t1 is None:
        t1 = time.time()
    return umsgpack.packb({"v": VERSION, "t1": t1}), t1


def pack_response(req, stratum, root_disp_ms, leap, t2=None, t3=None):
    """Server -> client.

    ``t2`` is the receive time and ``t3`` the send time, both on the *served*
    clock. A server that corrects its own clock offset passes the corrected
    values; otherwise time.time() at send is used. The caller should stamp t2
    as early as possible and pass it in ``req["_t2"]`` or as ``t2``.
    """
    if t2 is None:
        t2 = req.get("_t2")
    if t3 is None:
        t3 = time.time()
    return umsgpack.packb({
        "v": VERSION,
        "t1": req.get("t1"),
        "t2": t2,
        "t3": t3,
        "stratum": stratum,
        "root_disp_ms": root_disp_ms,
        "leap": leap,
    })


def unpack(data):
    return umsgpack.unpackb(data)


def compute(t1, t2, t3, t4):
    """RFC 5905 offset/delay. Positive offset = local clock is BEHIND server."""
    offset = ((t2 - t1) + (t3 - t4)) / 2.0
    delay = (t4 - t1) - (t3 - t2)
    return offset, delay


def best_sample(samples):
    """samples: list of (offset, delay, meta). Lowest-delay sample has the least
    queuing and asymmetry. This is the classic NTP filter for high-jitter paths."""
    return min(samples, key=lambda s: s[1]) if samples else None
