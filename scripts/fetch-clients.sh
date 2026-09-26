#!/bin/bash
# Download the LATEST STABLE release of each Reticulum client into dl/clients/
# for the read-only quick-start DVD. Versions are resolved dynamically from
# each project's GitHub "latest" (non-prerelease) release at run time -- nothing
# is version-pinned here, so re-running picks up new upstream releases.
#
# Where a release publishes checksums (per-file *.sha256 or a checksums file),
# each download is verified against them; otherwise the computed sha256 is
# recorded. A per-run dl/clients/MANIFEST.txt captures app/version/file/sha256.
set -u
cd "$(dirname "$(readlink -f "$0")")" || exit 90
DEST=dl/clients
LOG=/tmp/fetch-clients.log; : > "$LOG"; exec > >(tee -a "$LOG") 2>&1
echo "=== fetch-clients (latest stable) $(date) ==="
command -v python3 >/dev/null || { echo "FATAL: python3 required"; exit 90; }
command -v curl    >/dev/null || { echo "FATAL: curl required"; exit 90; }
mkdir -p "$DEST"

# 1) resolve latest-stable releases + curated asset lists.
#    Emits: BIN|app|version|url|filename   and   SUM|app|version|url|filename
PLAN="$DEST/.plan"
python3 - "$PLAN" <<'PY'
import json, urllib.request, urllib.error, sys
out = open(sys.argv[1], "w")
def api(url):
    req = urllib.request.Request(url, headers={"User-Agent": "rnsbox-fetch",
                                               "Accept": "application/vnd.github+json"})
    return json.load(urllib.request.urlopen(req, timeout=60))
def latest(repo):
    try:
        return api(f"https://api.github.com/repos/{repo}/releases/latest")
    except urllib.error.HTTPError:            # no stable release marked -> newest
        rels = api(f"https://api.github.com/repos/{repo}/releases?per_page=1")
        return rels[0] if rels else None
def pick(app, names):        # lean: one primary installer per OS family
    INST = ('.exe', '.msi', '.dmg', '.AppImage', '.appimage', '.deb', '.rpm', '.apk', '.zip', '.flatpak')
    names = [n for n in names if n.endswith(INST)]   # real installers only -- drop .blockmap/.cosign.bundle/.yml/.json/.sha256/.rsg sidecars
    e = lambda n, x: n.endswith(x)
    if app == "MeshChat":    # win installer, mac (arm64+x64), linux AppImage
        return [n for n in names if e(n, ('.AppImage', '.dmg')) or 'win-installer' in n]
    if app == "MeshChatX":   # win installer, mac dmg x2, linux AppImage x2, android apk
        return [n for n in names if n.startswith('ReticulumMeshChatX')
                and ('win-installer' in n or e(n, '.dmg') or e(n, '.AppImage')
                     or (e(n, '.apk') and 'android' in n))]
    if app == "Ratspeak":    # win setup, mac dmg x2, linux AppImage, android apk x3
        return [n for n in names if 'windows-x64-setup' in n or e(n, ('.dmg', '.AppImage', '.apk'))]
    if app == "Columba":     # android per-arch (official rns-py build)
        c = [n for n in names if e(n, '.apk') and 'official-rns-py' in n and 'no-sentry' not in n]
        return c or [n for n in names if e(n, '.apk') and not e(n, '.sha256')]
    if app == "Sideband":    # android apk, linux AppImage x2, win zip
        return [n for n in names if e(n, ('.apk', '.appimage', '.zip')) and not e(n, '.rsg')]
    return []
REPOS = [("MeshChat",  "liamcottle/reticulum-meshchat"),
         ("MeshChatX", "Quad4-Software/MeshChatX"),
         ("Ratspeak",  "ratspeak/Ratspeak"),
         ("Columba",   "torlando-tech/columba"),
         ("Sideband",  "markqvist/Sideband")]
rc = 0
for app, repo in REPOS:
    rel = latest(repo)
    if not rel:
        sys.stderr.write(f"!! {app}: no release found\n"); rc = 1; continue
    ver = rel["tag_name"]
    assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
    chosen = pick(app, list(assets))
    if not chosen:
        sys.stderr.write(f"!! {app}: no matching assets in {ver}\n"); rc = 1; continue
    sys.stderr.write(f"   {app}: {ver} -> {len(chosen)} binaries\n")
    for n in chosen:
        out.write(f"BIN|{app}|{ver}|{assets[n]}|{n}\n")
    for name, url in assets.items():
        if name.endswith('.sha256') or name.lower().startswith('checksums') or name == 'SHA256SUMS':
            out.write(f"SUM|{app}|{ver}|{url}|{name}\n")
out.close(); sys.exit(rc)
PY
[ $? -eq 0 ] || { echo "FATAL: could not resolve latest releases (network / GitHub API)"; exit 91; }

# 2) download binaries then checksum sidecars, into per-app subdirs
dl() { # url dir name  -> 0 ok / 1 fail
	local url="$1" dir="$2" name="$3" out="$2/$3"
	mkdir -p "$dir"
	if [ -s "$out" ]; then echo "  cached: $name"; return 0; fi
	echo "  get: $name"
	if curl -fL --retry 3 --retry-delay 2 -C - -o "$out" "$url"; then
		echo "    ok: $(du -h "$out" | cut -f1)"
	else
		echo "    FAILED: $name"; rm -f "$out"; return 1
	fi
}
FAIL=0
while IFS='|' read -r kind app ver url name; do
	[ "$kind" = BIN ] || continue
	dl "$url" "$DEST/$app" "$name" || FAIL=1
done < "$PLAN"
while IFS='|' read -r kind app ver url name; do
	[ "$kind" = SUM ] || continue
	dl "$url" "$DEST/$app" "$name" || true      # checksum files are best-effort
done < "$PLAN"

# 3) build a basename->sha256 index from any downloaded checksum files
IDX="$DEST/.shaidx"; : > "$IDX"
find "$DEST" -type f \( -name '*.sha256' -o -iname 'checksums*' -o -name 'SHA256SUMS' \) 2>/dev/null | while read -r cf; do
	awk 'NF>=2 && $1 ~ /^[0-9a-fA-F]{64}$/ {n=$2; sub(/.*\//,"",n); print $1"  "n}' "$cf" >> "$IDX"
	if [ "$(wc -w < "$cf")" -eq 1 ]; then      # bare-hash "<file>.sha256"
		printf '%s  %s\n' "$(tr -d '[:space:]' < "$cf")" "$(basename "$cf" .sha256)" >> "$IDX"
	fi
done

# 4) verify against the index where possible; record everything in the manifest
MAN="$DEST/MANIFEST.txt"; : > "$MAN"
echo "# RNSBox client binaries -- fetched $(date -u)" >> "$MAN"
echo "# app  version  file  sha256  status" >> "$MAN"
while IFS='|' read -r kind app ver url name; do
	[ "$kind" = BIN ] || continue
	f="$DEST/$app/$name"
	[ -s "$f" ] || { echo "  MISSING: $app/$name"; FAIL=1; continue; }
	sha=$(sha256sum "$f" | cut -d' ' -f1)
	want=$(awk -v n="$name" '$2==n{print $1; exit}' "$IDX")
	if [ -n "$want" ]; then
		[ "$want" = "$sha" ] && st=VERIFIED || { st=MISMATCH; FAIL=1; echo "  !! CHECKSUM MISMATCH: $app/$name"; }
	else
		st=RECORDED
	fi
	printf '%s  %s  %s  %s  %s\n' "$app" "$ver" "$name" "$sha" "$st" >> "$MAN"
done < "$PLAN"
rm -f "$PLAN" "$IDX"

echo "=== manifest ($MAN) ==="; cat "$MAN"
echo "=== per-app totals ==="; du -sh "$DEST"/*/ 2>/dev/null
echo "=== grand total ==="; du -sh "$DEST"
[ "$FAIL" = 0 ] || { echo "FETCH-FAIL"; exit 92; }
echo "FETCH-DONE-SENTINEL"
