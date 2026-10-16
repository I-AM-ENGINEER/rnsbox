"""generic provider — any HTTP(S) site serving a manifest.json.

Covers the "other sites, if the user wants" case: conf["url"] is the FULL
URL of the manifest; asset URLs inside it may be absolute or relative to
the manifest URL. Schema (see providers README):

    {
      "version": "r2026.10.15-abc1234",
      "files": [
        {"board": "sg2002-rv64", "url": "rnsbox-sg2002-rv64.img.xz",
         "sha256": "<64 hex>"}
      ]
    }

Optional ed25519 signature (sig_require=yes): manifest.json.sig next to the
manifest holds base64(ed25519_sign(pubkey, manifest.json bytes)). The pubkey
is pinned in the image (sig_key). Verification uses python-cryptography when
configured — the check is skipped ONLY when sig_require=no.
"""

import base64
import json
import re
import urllib.parse

SHA_RE = re.compile(r"^[0-9a-fA-F]{64}$")


def fetch(conf, board, fetch_url, truthy):
    manifest_url = (conf.get("url") or "").strip()
    if not manifest_url:
        raise ValueError("update.conf: url (manifest.json) is empty — set it "
                         "in the portal Updates tab")
    raw = fetch_url(manifest_url, conf, timeout=30)
    if isinstance(raw, bytes):
        raw_bytes = raw
        text = raw.decode("utf-8", "replace")
    else:
        raw_bytes = raw.encode("utf-8")
        text = raw

    if truthy(conf.get("sig_require")):
        verify_signature(conf, manifest_url, raw_bytes, fetch_url)
    try:
        man = json.loads(text)
    except ValueError as e:
        raise ValueError("manifest is not valid JSON: %s" % e)
    version = str(man.get("version") or "").strip()
    if not re.match(r"^[\w.-]{1,64}$", version):
        raise ValueError("manifest: missing/invalid version")

    for f in man.get("files") or []:
        if str(f.get("board")) != board:
            continue
        sha = str(f.get("sha256") or "").lower()
        if not SHA_RE.match(sha):
            raise ValueError("manifest: board %s entry has no sha256 — "
                             "checksums are mandatory" % board)
        url = str(f.get("url") or "")
        absolute = urllib.parse.urljoin(manifest_url, url)
        return {"version": version, "url": absolute, "sha256": sha,
                "prerelease": bool(man.get("prerelease"))}
    raise ValueError("manifest %s lists no file for board %s"
                     % (version, board))


def verify_signature(conf, manifest_url, raw, fetch_url):
    sig_url = manifest_url.rsplit("?", 1)[0] + ".sig"
    try:
        sig_b64 = fetch_url(sig_url, conf, timeout=20)
    except Exception as e:
        raise ValueError("signature required but %s.fetch failed: %s"
                         % (sig_url.rsplit("/", 1)[-1], e))
    if isinstance(sig_b64, bytes):
        sig_b64 = sig_b64.decode("ascii", "replace")
    key_path = conf.get("sig_key") or "/etc/rnsbox/keys/update.pub"
    try:
        with open(key_path, "rb") as f:
            pubkey_b64 = "".join(
                l for l in f.read().decode("ascii", "replace").splitlines()
                if l and not l.startswith(("#", "-----")))
        pubkey = base64.b64decode("".join(pubkey_b64.split()), validate=True)
        sig = base64.b64decode("".join(sig_b64.split()), validate=True)
    except Exception as e:
        raise ValueError("cannot load update signing key %s: %s"
                         % (key_path, e))
    try:
        from cryptography.hazmat.primitives.asymmetric import ed25519
        from cryptography.exceptions import InvalidSignature
        ed25519.Ed25519PublicKey.from_public_bytes(pubkey).verify(
            sig, raw)
    except InvalidSignature:
        raise ValueError("manifest signature INVALID — not updating")
    except ImportError:
        raise ValueError("signature required but python-cryptography is not "
                         "installed")
