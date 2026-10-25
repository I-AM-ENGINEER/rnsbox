"""MeshAPI 0.1 manifest for rns-time.

Served over RNS via the __manifest__ discovery op on the MeshAPI request path.
Plain dict (conforms to MeshAPI 0.1). See github.com/wdunn001/meshapi.

Note: the precise clock sync is the packet-based NTP 4-timestamp exchange
(see protocol.py / the client); MeshAPI here exposes the simple
request/response `now` op for programmatic clients and the interactive
explorer.
"""
import copy

_BASE = {
    "meshapi": "0.1",
    "service": {
        "name": "rns-time",
        "summary": "Authenticated time over Reticulum",
        "app": "rnstime",
        "aspect": "time",
        "path": "t",
        "dest": "",
        "encoding": "umsgpack",
        "source": "https://github.com/wdunn001/rns-time",
    },
    "ops": [
        {"op": "now", "summary": "Current server time", "auth": "none",
         "request": {},
         "response": {"utc": "str", "epoch": "float", "stratum": "int",
                      "root_disp_ms": "float", "leap": "str"}},
    ],
}


def manifest_for(dest_hex):
    """The manifest with this server's destination hash filled in."""
    manifest = copy.deepcopy(_BASE)
    manifest["service"]["dest"] = dest_hex
    return manifest


# Kept for compatibility with the original module: the reference server's
# hash. A running server uses manifest_for(own_hash) instead.
MANIFEST = manifest_for("e210fb416af335bd68a9ed4b86919895")
