"""github-releases provider — latest rnsbox-<board>.img.xz from a repo's
GitHub Releases, checksum verified against SHA256SUMS or the release notes.

Adding a provider = a module here exposing fetch(conf, board, fetch_url,
truthy) -> {"version", "url", "sha256", "prerelease"}.

Notes:
  * unauthenticated API (60 req/h per IP) is plenty for a 7-14 day cadence;
    conf["token"] enables higher limits / private repos.
  * our CI (build.yml) publishes ONE release per run: assets
    rnsbox-<board>.img.xz and NO SHA256SUMS asset — the per-file sha256 lives
    in the notes as a markdown table row: "| board | asset | sha256 |".
    This provider parses that table; a SHA256SUMS asset is used when present
    (other repos, future CI).
"""

import json
import re
import urllib.parse

API = "https://api.github.com/repos/{repo}/releases?per_page=15"
NOTES_ROW = re.compile(
    r"^\|\s*[^|]+\|\s*`?([^`|]+?)`?\s*\|\s*`?([0-9a-fA-F]{64})`?\s*\|",
    re.MULTILINE)


def _api_get(url, conf, fetch_url, token):
    saved = conf.get("token") or ""
    if token:
        conf["token"] = token
    try:
        return json.loads(fetch_url(url, conf, timeout=20))
    finally:
        conf["token"] = saved


def _checksums_from_notes(body):
    out = {}
    for m in NOTES_ROW.finditer(body or ""):
        out[m.group(1).strip()] = m.group(2).lower()
    return out


def _checksums_from_sums_asset(asset, conf, fetch_url):
    text = fetch_url(asset["browser_download_url"], conf, timeout=30)
    if isinstance(text, bytes):
        text = text.decode("utf-8", "replace")
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2:
            out[parts[1].lstrip("*")] = parts[0].lower()
    return out


def fetch(conf, board, fetch_url, truthy):
    repo = (conf.get("repo") or "").strip()
    if not re.match(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$", repo):
        raise ValueError("update.conf: repo must look like owner/repo")
    url = API.format(repo=urllib.parse.quote(repo, safe="/"))
    token = (conf.get("token") or "").strip()
    rels = _api_get(url, conf, fetch_url, token)

    want_pre = truthy(conf.get("allow_prerelease"), True)
    for rel in rels:
        if rel.get("draft"):
            continue
        pre = bool(rel.get("prerelease"))
        if pre and not want_pre:
            continue
        assets = {a["name"]: a for a in rel.get("assets") or []}
        asset = assets.get("rnsbox-%s.img.xz" % board)
        if not asset:
            continue
        sha = ""
        sums = next((assets[n] for n in assets
                     if n.lower() in ("sha256sums", "sha256sums.txt")), None)
        if sums:
            try:
                sha = _checksums_from_sums_asset(sums, conf, fetch_url).get(
                    asset["name"], "")
            except Exception:
                sha = ""
        if not sha:
            sha = _checksums_from_notes(rel.get("body") or "").get(
                asset["name"], "")
        if not sha:
            raise ValueError(
                "release %s has no verifiable sha256 for %s — refusing "
                "(checksums are mandatory)" % (rel.get("tag_name"),
                                               asset["name"]))
        return {"version": rel.get("tag_name") or "",
                "url": asset["browser_download_url"],
                "sha256": sha,
                "prerelease": pre}
    raise ValueError("no release with a rnsbox-%s.img.xz asset%s" % (
        board, "" if want_pre else " (allow_prerelease=yes would widen "
        "the search)"))
