# rnsbox-sysupdate providers

A provider is one python module in this directory exposing:

    fetch(conf, board, fetch_url, truthy) ->
        {"version": str, "url": str, "sha256": 64-hex, "prerelease": bool}

`conf` is the parsed /etc/rnsbox/update.conf (plus DEFAULTS); `board` is the
resolved board id used for asset selection; `fetch_url(url, conf, timeout=…,
dest=None, max_bytes=None)` is the worker's HTTPS-enforcing getter (bytes, or
byte count when `dest` is set); `truthy(v, default)` parses yes/no.

Register the module name in `rnsbox-sysupdate` (registry comment in
load_provider / the portal's provider dropdown) — nothing else.

Checksums are mandatory: a provider that cannot return a sha256 must raise;
the worker refuses to apply without one.

## generic manifest schema

`conf["url"]` points at a manifest.json; file URLs may be relative:

    {
      "version": "r2026.10.15-abc1234",
      "prerelease": false,
      "files": [
        {"board": "licheerv-nano", "url": "rnsbox-licheerv-nano.img.xz",
         "sha256": "…64 hex…"},
        {"board": "rpi-universal", "url":
         "https://mirror.example.net/rnsbox/rnsbox-rpi-universal.img.xz",
         "sha256": "…"}
      ]
    }

Optional signing (sig_require=yes): serve `manifest.json.sig` containing
base64(ed25519 bytes of the exact manifest.json body); pin the matching
base64 raw public key in /etc/rnsbox/keys/update.pub (`sig_key`).
