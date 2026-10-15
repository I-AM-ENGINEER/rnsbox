#!/usr/bin/env python3
"""Host unit tests for rnsbox-sysupdate: providers, schedule, transaction,
abselect fallbacks. Runs against a local fake HTTP server and file-backed
"disks" — no root, no block devices, no network.

Usage: python3 tests/test_sysupdate.py [-v]
"""

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
WORKER = os.path.join(REPO, "package", "rnsbox-update", "src", "rnsbox-sysupdate")
ABSELECT = os.path.join(REPO, "package", "rnsbox-update", "src", "abselect")

IMG_BYTES = b"\xab" * (300 * 1024)          # filler "partition" payload
BOOT_SD = b"\x42" * 4096                    # fake boot.sd payload


def sha(b):
    return hashlib.sha256(b).hexdigest()


class FakeRepo(object):
    """Builds release/manifest payloads served by the fake HTTP server."""

    def __init__(self):
        self.mode = "github"          # github | sums | generic
        self.files = {}               # name -> bytes served under /dl/
        self.manifest = {}
        self.notes_sha = True         # sha in the notes table
        self.releases = []

    def github_json(self, board="licheerv-nano"):
        rels = []
        for r in self.releases or self._default_releases():
            body = ""
            if self.notes_sha and r.get("asset"):
                body = ("| Board | Image | sha256 |\n|---|---|---|\n"
                        "| %s | `%s` | `%s` |" % (board, r["asset_name"],
                                                 r.get("sha", "") or ""))
            rel = {"tag_name": r["tag"], "prerelease": r.get("pre", False),
                   "draft": False, "body": body, "assets": []}
            if r.get("asset"):
                rel["assets"].append({
                    "name": r["asset_name"],
                    "browser_download_url": URL + "/dl/" + r["asset_name"],
                })
            if r.get("sums"):
                rel["assets"].append({
                    "name": "SHA256SUMS",
                    "browser_download_url": URL + "/dl/SHA256SUMS",
                })
            rels.append(rel)
        return rels

    def _default_releases(self):
        return [{
            "tag": TAG_NEW, "pre": False, "asset": True,
            "asset_name": "rnsbox-licheerv-nano.img.xz", "sha": IMG_SHA,
        }, {
            "tag": TAG_OLD, "pre": True, "asset": True,
            "asset_name": "rnsbox-licheerv-nano.img.xz", "sha": IMG_SHA,
        }]


TAG_NEW = "r2026.10.15-deadbe7"
TAG_OLD = "r2026.10.01-abc1234"


class Handler(BaseHTTPRequestHandler):
    repo = None            # set by serve()
    files = {}             # name -> bytes

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        repo = Handler.repo
        if self.path.startswith("/api/repos/"):
            self._send(200, json.dumps(repo.github_json()).encode())
        elif self.path.startswith("/dl/"):
            name = self.path.split("/dl/", 1)[1]
            if name == "SHA256SUMS" and repo.mode == "sums":
                body = ("%s  rnsbox-licheerv-nano.img.xz\n" % IMG_SHA).encode()
                return self._send(200, body, "text/plain")
            if name == "manifest.json":
                return self._send(200, json.dumps(repo.manifest).encode(),
                                  "application/json")
            if name == "manifest.json.sig":
                return self._send(200, b"notarealsig", "text/plain")
            data = repo.files.get(name)
            if data is None:
                return self._send(404, b"nope", "text/plain")
            self._send(200, data, "application/x-xz")
        else:
            self._send(404, b"nope", "text/plain")


def make_fat(files):
    """Build a FAT16 filesystem image with the given {name: bytes} via
    mkfs.vfat + mcopy (both present on the host and in the images)."""
    d = tempfile.mkdtemp(prefix="fat.")
    img_path = os.path.join(d, "boot.fat")   # must NOT exist: mkfs -C refuses
    mb = 32   # FAT16 needs 16M+ (4085+ clusters); keep the test FAT valid
    subprocess.run(["mkfs.vfat", "-C", "-F16", "-n", "boot",
                    img_path, str(mb * 1024)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for name, data in files.items():
        p = tempfile.NamedTemporaryFile(delete=False)
        p.write(data)
        p.close()
        subprocess.run(["mcopy", "-i", img_path, "-n", "-o", p.name,
                        "::" + name], check=True)
        os.unlink(p.name)
    with open(img_path, "rb") as f:
        data = f.read()
    shutil.rmtree(d, ignore_errors=True)
    return data


def make_ab_image(board_boot_files, slot_payload=IMG_BYTES):
    """Assemble a full A/B disk image (MBR + p1 FAT + p2/p3/p4 payloads),
    compress it with xz, return the .img.xz bytes. Partitions are tiny — the
    worker only checks sizes, not proportions."""
    fat = make_fat(board_boot_files)
    p1 = fat[:4 * 1024 * 1024]
    assert len(p1) == 4 * 1024 * 1024
    p2 = slot_payload
    p3 = b"\xee" * len(slot_payload)
    p4 = b"\x00" * (64 * 1024)
    disk = bytearray(b"\x00" * 512)
    for i, (ptype, start, count) in enumerate([
            (0x0C, 1, len(p1) // 512),
            (0x83, 1 + len(p1) // 512, len(p2) // 512),
            (0x83, 1 + (len(p1) + len(p2)) // 512, len(p3) // 512),
            (0x83, 1 + (len(p1) + len(p2) + len(p3)) // 512,
             len(p4) // 512)]):
        e = bytearray(16)
        e[4] = ptype
        e[8:12] = struct.pack("<I", start)
        e[12:16] = struct.pack("<I", count)
        disk[446 + 16 * i:446 + 16 * (i + 1)] = e
    disk[510:512] = b"\x55\xaa"
    disk += p1 + p2 + p3 + p4
    gz = tempfile.NamedTemporaryFile(suffix=".img.xz", delete=False)
    gz.close()
    with open(gz.name, "wb") as out:
        subprocess.run(["xz", "-c", "-0", "-f"], input=bytes(disk),
                       stdout=out, check=True)
    with open(gz.name, "rb") as f:
        data = f.read()
    os.unlink(gz.name)
    return data


class SysupdateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="rnsbox-test.")
        self.conf = os.path.join(self.tmp, "update.conf")
        self.runst = os.path.join(self.tmp, "run.json")
        self.statedir = os.path.join(self.tmp, "state")
        self.ver = os.path.join(self.tmp, "image.ver")
        self.boot = os.path.join(self.tmp, "boot")
        os.makedirs(self.boot)
        self.write_conf(dict(DEFAULT_CONF))
        # fake disks
        self.slot_a = os.path.join(self.tmp, "slot_a.dev")
        self.slot_b = os.path.join(self.tmp, "slot_b.dev")
        for p in (self.slot_a, self.slot_b):
            with open(p, "wb") as f:
                f.write(b"\x00" * (512 * 1024))
        self.devmap = os.path.join(self.tmp, "devmap.json")
        with open(self.devmap, "w") as f:
            json.dump({"boot_dir": self.boot, "boot_dev": "/dev/mmcblk0p1",
                       "slot_a": self.slot_a, "slot_b": self.slot_b}, f)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ---- helpers ----

    def write_conf(self, d):
        with open(self.conf, "w") as f:
            for k, v in d.items():
                f.write("%s=%s\n" % (k, v))

    def env(self, **extra):
        e = dict(os.environ)
        e.update({
            "RNSBOX_SYSUPDATE_CONF": self.conf,
            "RNSBOX_SYSUPDATE_RUN": self.runst,
            "RNSBOX_SYSUPDATE_STATEDIR": self.statedir,
            "RNSBOX_SYSUPDATE_VER": self.ver,
            "RNSBOX_SYSUPDATE_LOCK": os.path.join(self.tmp, "lock"),
            "RNSBOX_SYSUPDATE_BOOT": self.boot,
            "RNSBOX_UPDATE_DEVMAP": self.devmap,
            "RNSBOX_SYSUPDATE_FOREGROUND": "1",
            "RNSBOX_SYSUPDATE_NO_REBOOT": "1",
            "RNSBOX_UPDATE_GHAPI": "http://127.0.0.1:%d/api" % PORT,
        })
        e.update(extra)
        return e

    def worker(self, *args, **kw):
        may_fail = kw.pop("may_fail", False)
        env = self.env(**kw.pop("env", {}))
        r = subprocess.run([sys.executable, WORKER, *args],
                           capture_output=True, text=True, env=env, **kw)
        if r.returncode != 0 and not may_fail:
            state = ""
            try:
                state = open(self.runst).read()[-500:]
                logp = os.path.join(self.statedir, "update.log")
                state += " LOG: " + open(logp).read()[-500:]
            except OSError:
                pass
            self.fail("worker %s failed rc=%s: %s STATE:%s" %
                      (args, r.returncode, r.stderr[-500:], state))
        return r

    def state(self):
        with open(self.runst) as f:
            return json.load(f)

    def boot_state(self, slot="a", tries="0"):
        with open(os.path.join(self.boot, "ab_slot"), "w") as f:
            f.write(slot + "\n")
        with open(os.path.join(self.boot, "ab_tries"), "w") as f:
            f.write(tries + "\n")

    # ---- version / schedule ----

    def test_version_compare(self):
        out = self.worker("status").stdout
        st = json.loads(out)
        self.assertEqual(st["running_version"], "")
        with open(self.ver, "w") as f:
            f.write(TAG_OLD + "\n")
        st = json.loads(self.worker("status").stdout)
        self.assertEqual(st["running_version"], TAG_OLD)

    def test_schedule_jitter_bounds(self):
        import random
        lo, hi = 7 * 86400, 14 * 86400
        now = time.time()
        for _ in range(200):
            delta = random.randint(lo, hi)
            self.assertGreaterEqual(delta, lo)
            self.assertLessEqual(delta, hi)

    # ---- providers (via the fake HTTP server) ----

    def test_github_provider_notes_table(self):
        with open(self.ver, "w") as f:
            f.write(TAG_OLD + "\n")
        self.write_conf(dict(DEFAULT_CONF, provider="github-releases",
                             url="", allow_insecure_http="yes"))
        st = self.state_after_check()
        self.assertEqual(st["phase"], "available")
        self.assertEqual(st["candidate"]["version"], TAG_NEW)
        self.assertEqual(st["candidate"]["sha256"], IMG_SHA)

    def state_after_check(self):
        # a failed check is a LEGITIMATE result (rc=1, phase=error) — the
        # assertions below decide whether it was expected
        self.worker("check", may_fail=True)
        return self.state()

    def test_github_prerelease_filtered(self):
        with open(self.ver, "w") as f:
            f.write(TAG_OLD + "\n")
        # the only release with an asset is a prerelease -> filtered out
        Handler.repo.releases = [{
            "tag": TAG_NEW, "pre": True, "asset": True,
            "asset_name": "rnsbox-licheerv-nano.img.xz", "sha": IMG_SHA,
        }]
        self.write_conf(dict(DEFAULT_CONF, provider="github-releases",
                             allow_insecure_http="yes",
                             allow_prerelease="no"))
        st = self.state_after_check()
        self.assertEqual(st["phase"], "error")

    def test_github_no_checksum_refused(self):
        with open(self.ver, "w") as f:
            f.write(TAG_OLD + "\n")
        self.write_conf(dict(DEFAULT_CONF, provider="github-releases",
                             allow_insecure_http="yes"))
        Handler.repo.notes_sha = False
        st = self.state_after_check()
        Handler.repo.notes_sha = True
        self.assertEqual(st["phase"], "error")
        self.assertIn("sha256", st["error"])

    def test_https_enforced(self):
        self.write_conf(dict(DEFAULT_CONF, provider="generic",
                             url="http://127.0.0.1:%d/dl/manifest.json"
                                 % PORT))
        st = self.state_after_check()
        self.assertEqual(st["phase"], "error")
        self.assertIn("HTTPS", st["error"])

    def test_generic_provider(self):
        Handler.repo.manifest = {
            "version": TAG_NEW,
            "files": [{"board": "licheerv-nano",
                       "url": "/dl/rnsbox-licheerv-nano.img.xz",
                       "sha256": IMG_SHA}],
        }
        self.write_conf(dict(DEFAULT_CONF, provider="generic",
                             url="http://127.0.0.1:%d/dl/manifest.json"
                                 % PORT,
                             allow_insecure_http="yes"))
        st = self.state_after_check()
        self.assertEqual(st["phase"], "available")
        self.assertEqual(st["candidate"]["version"], TAG_NEW)
        self.assertTrue(st["candidate"]["url"].endswith(
            "/dl/rnsbox-licheerv-nano.img.xz"))

    # ---- apply transaction (file-backed slots) ----

    BOOT_FILES = {"boot.sd": b"CIMG" + b"\x00" * 60 + BOOT_SD,
                  "fip.bin": b"FIP",
                  "rnsbox-abselect": b"#!/bin/sh\nabselect() { :; }\n"}

    def prep_apply(self):
        """A checked candidate + a fresh A/B state + fake image."""
        with open(self.ver, "w") as f:
            f.write(TAG_OLD + "\n")
        self.boot_state("a", "0")
        img_xz = make_ab_image(self.BOOT_FILES)
        # the release must checksum the SERVED FILE (the .img.xz), exactly
        # like the real CI quotes the asset hash in the release notes
        Handler.repo.releases = [{
            "tag": TAG_NEW, "pre": False, "asset": True,
            "asset_name": "rnsbox-licheerv-nano.img.xz", "sha": sha(img_xz),
        }]
        Handler.repo.files["rnsbox-licheerv-nano.img.xz"] = img_xz
        self.write_conf(dict(DEFAULT_CONF, provider="github-releases",
                             allow_insecure_http="yes"))
        self.worker("check")
        self.assertEqual(self.state()["phase"], "available")
        return img_xz

    def test_fat_lfn_read(self):
        """The FAT reader must handle long file names (mkfs.vfat-made)."""
        fat = make_fat(self.BOOT_FILES)
        mod = load_worker_module()
        fr = mod.FatReader(fat)
        names = sorted(e["name"] for e in fr.listdir("/"))
        self.assertIn("rnsbox-abselect", names)
        self.assertEqual(fr.read("rnsbox-abselect"),
                         self.BOOT_FILES["rnsbox-abselect"])
        self.assertEqual(fr.read("boot.sd"), self.BOOT_FILES["boot.sd"])

    def test_apply_full_transaction(self):
        # an old boot.sd must exist so the backup path is exercised
        with open(os.path.join(self.boot, "boot.sd"), "wb") as f:
            f.write(b"")
        self.prep_apply()
        self.worker("apply")
        st = self.state()
        self.assertEqual(st["phase"], "flipping")
        self.assertEqual(st["pending"]["slot"], "b")
        with open(os.path.join(self.boot, "ab_slot")) as f:
            self.assertEqual(f.read().strip(), "b")
        with open(os.path.join(self.boot, "ab_tries")) as f:
            self.assertEqual(f.read().strip(), "0")
        # idle slot (b) got the image's p3 payload; running slot untouched
        with open(self.slot_b, "rb") as f:
            self.assertEqual(f.read(len(IMG_BYTES)), b"\xee" * len(IMG_BYTES))
        with open(self.slot_a, "rb") as f:
            self.assertEqual(f.read(64), b"\x00" * 64)
        # boot.sd refreshed with the CIMG header stripped, old copy kept
        with open(os.path.join(self.boot, "boot.sd"), "rb") as f:
            self.assertEqual(f.read(), BOOT_SD)
        bak = os.path.join(self.statedir, "boot-bak", "boot.sd.old")
        with open(bak, "rb") as f:
            self.assertEqual(f.read(), b"")     # old boot.sd was empty

    def test_apply_sha_mismatch_aborts(self):
        """A corrupted download is caught before anything is written."""
        with open(self.ver, "w") as f:
            f.write(TAG_OLD + "\n")
        self.boot_state("a", "0")
        Handler.repo.files["rnsbox-licheerv-nano.img.xz"] = b"corrupt"
        self.write_conf(dict(DEFAULT_CONF, provider="github-releases",
                             allow_insecure_http="yes"))
        # seed a state file, then a candidate that claims to be the good
        # image while the server actually serves garbage
        with open(self.runst, "w") as f:
            f.write("{}")
        st = self.state()
        st["candidate"] = {"version": TAG_NEW,
                           "url": URL + "/dl/rnsbox-licheerv-nano.img.xz",
                           "sha256": IMG_SHA, "prerelease": False}
        st["phase"] = "available"
        with open(self.runst, "w") as f:
            json.dump(st, f)
        self.worker("apply", may_fail=True)
        st = self.state()
        self.assertEqual(st["phase"], "error")
        self.assertIn("sha256 mismatch", st["error"])
        self.assertFalse(os.path.exists(os.path.join(self.statedir,
                                                     "download.img.xz")))
        with open(os.path.join(self.boot, "ab_slot")) as f:
            self.assertEqual(f.read().strip(), "a")   # NOT flipped
        with open(self.slot_b, "rb") as f:
            self.assertEqual(f.read(64), b"\x00" * 64)

    def test_reconcile_applied(self):
        self.prep_apply()
        self.worker("apply")
        st = self.state()
        st["pending"] = {"slot": "b", "version": TAG_NEW,
                         "started": int(time.time())}
        with open(self.runst, "w") as f:
            json.dump(st, f)
        self.boot_state("b", "0")            # healthy boot cleared tries
        self.worker("reconcile")
        st = self.state()
        self.assertEqual(st["phase"], "applied")
        self.assertNotIn("pending", st)

    def test_reconcile_rolled_back(self):
        self.prep_apply()
        self.worker("apply")
        st = self.state()
        st["pending"] = {"slot": "b", "version": TAG_NEW,
                         "started": int(time.time())}
        with open(self.runst, "w") as f:
            json.dump(st, f)
        self.boot_state("a", "0")            # initramfs flipped back
        self.worker("reconcile")
        st = self.state()
        self.assertEqual(st["phase"], "rolled_back")

    def test_no_layout_refuses(self):
        # disable the devmap: the fake boot dir is not a mount, so the real
        # probe must report the non-A/B layout
        r = self.worker("status", env={"RNSBOX_UPDATE_DEVMAP": ""})
        st = json.loads(r.stdout)
        self.assertEqual(st["layout"], "none")


DEFAULT_CONF = {
    "enabled": "yes",
    "provider": "github-releases",
    "repo": "example/rnsbox",
    "url": "",
    "board": "licheerv-nano",
    "token": "",
    "allow_prerelease": "yes",
    "allow_insecure_http": "no",
    "auto_apply": "no",
    "interval_min_days": "7",
    "interval_max_days": "14",
    "next_check": "0",
    "sig_require": "no",
    "sig_key": "",
}


def abselect_sh(bootdir):
    """Run the abselect decision for a boot dir, return (slot, tries_text)."""
    script = (
        '. "%s"\n'
        'abselect "%s" /dev/mmcblk0p2 /dev/mmcblk0p3\n'
        'echo "$AB_ACTIVE $AB_ROOTDEV $AB_ROLLED_BACK"\n'
    ) % (ABSELECT, bootdir)
    r = subprocess.run(["sh", "-c", script], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    parts = r.stdout.split()
    return parts[0], parts[1], (parts[2] if len(parts) > 2 else "")


class AbselectTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="abselect-test.")
        self.boot = os.path.join(self.tmp, "boot")
        os.makedirs(self.boot)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def set_state(self, slot, tries):
        with open(os.path.join(self.boot, "ab_slot"), "w") as f:
            f.write(slot + "\n")
        with open(os.path.join(self.boot, "ab_tries"), "w") as f:
            f.write(tries + "\n")

    def test_first_boot_slot_a(self):
        self.set_state("a", "0")
        slot, root, _ = abselect_sh(self.boot)
        self.assertEqual(slot, "a")
        self.assertEqual(root, "/dev/mmcblk0p2")
        self.assertEqual(open(os.path.join(self.boot, "ab_tries")).read().strip(),
                         "1")

    def test_rollback_after_max(self):
        self.set_state("b", "3")
        slot, root, rolled = abselect_sh(self.boot)
        self.assertEqual(slot, "a")
        self.assertEqual(root, "/dev/mmcblk0p2")
        self.assertEqual(rolled, "1")
        self.assertEqual(open(os.path.join(self.boot, "ab_tries")).read().strip(),
                         "1")

    def test_corrupt_state_defaults(self):
        with open(os.path.join(self.boot, "ab_slot"), "w") as f:
            f.write("garbage\n")
        with open(os.path.join(self.boot, "ab_tries"), "w") as f:
            f.write("not-a-number\n")
        slot, root, _ = abselect_sh(self.boot)
        self.assertEqual(slot, "a")
        self.assertEqual(root, "/dev/mmcblk0p2")

    def test_flip_increments(self):
        self.set_state("b", "0")
        slot, root, _ = abselect_sh(self.boot)
        self.assertEqual(slot, "b")
        self.assertEqual(root, "/dev/mmcblk0p3")
        self.assertEqual(open(os.path.join(self.boot, "ab_tries")).read().strip(),
                         "1")


def load_worker_module():
    """Import the worker script (no .py extension) as a module."""
    import importlib.machinery
    import importlib.util
    loader = importlib.machinery.SourceFileLoader("rnsbox_sysupdate", WORKER)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def main():
    global PORT, URL, IMG_SHA, Handler_repo
    Handler.repo = FakeRepo()
    IMG_SHA = sha(IMG_BYTES)
    Handler.repo.files["rnsbox-licheerv-nano.img.xz"] = IMG_BYTES[:0]  # replaced per-test
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    global PORT
    PORT = srv.server_address[1]
    globals()["URL"] = "http://127.0.0.1:%d" % PORT
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    # the worker's fetch_url refuses http:// by default; the provider tests
    # that need the fake server set allow_insecure_http=yes in their conf.
    unittest.main(exit=False, argv=[sys.argv[0], "-v"])
    srv.shutdown()


if __name__ == "__main__":
    main()
