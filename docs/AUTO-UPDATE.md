# RNSBox auto-update: design and user guide

Status: implemented on branch `auto-update` (base: `raspberry-pi-zero-2w`).
Upstream groundwork: `upstream/auto-updater` (`7fa2cfb`, HW-validated A/B boot
machinery on the LicheeRV Nano) — Phase 1+2 ported, Phase 3 (the actual update
agent) written here as the `rnsbox-update` buildroot package.

## 1. Update model — A/B whole-OS image (chosen)

**Choice: full-image A/B slots.** The device keeps two complete rootfs
partitions (rootfs_A = p2, rootfs_B = p3) plus a shared boot partition (p1) and
a persistent data partition (p4). An update writes the *idle* slot from the
downloaded whole-OS image, then flips a slot pointer. The boot chain picks the
active slot and auto-reverts after 3 unconfirmed boots.

Why A/B, and not the alternatives:

- **(a) boot.sd-only + rootfs tarball applied live** — updating the ext4 that
  is currently mounted is exactly the "power cut = dead router" scenario we
  must never produce. A tarball transaction can be interrupted between files;
  A/B cannot (the running slot is never touched).
- **(c) boot.sd-only updates only** — kernels move between releases (module
  vermagic, DTBs); a rootfs that matches an old kernel is a second-class
  update. CI publishes whole images anyway, so whole-image updates are
  compatible-by-construction with what we ship.
- **(b) wins** because the hard part (slot selection + boot-count rollback +
  health confirmation) already exists upstream and is HW-validated.

Costs, accepted and documented:

- Layout change → **one reflash** adopts it; existing single-rootfs cards
  (and the dvd variant) are detected (`no p3`) and the UI says "reflash with a
  current image once, then updates are over-the-air".
- Image raw size roughly doubles (two ~1.5 GiB slots for licheerv-nano, two
  512 MiB for rpi0-2w); the published `.img.xz` grows only by a second copy of
  the OS content (empty slot space is zeros → xz), i.e. roughly 2x asset size.
- SD card: **≥ 4 GiB recommended** for licheerv-nano (2 x 1.5 GiB slots + boot
  + data), ≥ 1.5 GiB for rpi0-2w.

Shared-boot residual risk: p1 (boot.sd on cvitek, kernel/dtb/firmware on rpi)
is updated in place, after the new slot is fully written and verified but
before the slot flip. A power cut inside this seconds-wide window can damage
the boot file; the old copy is kept on /data and restored automatically on
hash mismatch. This matches the risk level the upstream HW-validated flow
already accepts for `boot.sd`.

### What is ported vs. new

| Piece | Origin |
|---|---|
| 4-partition genimage (`genimage_rootless_ab.cfg`), S03data (persistent /data + state bind-mounts + swap), S99resizefs A/B mode | upstream `3e6fe92` patch 0023, ported as series patch `0025` (slot size 1024M → 1536M: this branch's lite ext4 is 1500M, not upstream's ≤1 GiB) |
| initramfs slot selector + boot-count rollback (AB_MAX=3) + unmountable-slot fallback, S98confirm health confirm (rnsd up + portal :80), timing fix | upstream `3e6fe92` patch 0024 + `7fa2cfb`, ported as series patch `0026`, extended: the selector logic is sourced from `/boot/rnsbox-abselect` (single source, unit/QEMU-testable) with the validated upstream logic as inline fallback |
| Providers, scheduler (7–14 day jitter), download/verify/apply transaction, `/etc/rnsbox/update.conf`, portal "Updates" tab, image version stamp | new (`package/rnsbox-update`, portal routes, build.sh version stamp) |

RPi0-2W: same model, implemented in the external-tree build
(`genimage` A/B layout, tiny busybox initramfs built at post-image via the
kernel's `gen_init_cpio`, per-slot rootfs). **Not yet hardware-validated**
(QEMU here covers the transaction; the boot path needs a real Pi — see §8).

## 2. Providers

Config lives in `/etc/rnsbox/update.conf` (flat `key=value`, read/written by
both the CGI (`store.h`) and the python worker):

```
enabled=yes                 # auto-check master switch (default yes, see §3)
provider=github-releases    # github-releases | generic
repo=I-AM-ENGINEER/rnsbox   # github-releases: owner/repo
url=                        # generic: full URL of manifest.json
board=auto                  # auto | rpi0-2w | licheerv-nano (asset selector)
allow_prerelease=yes        # see §3
allow_insecure_http=no      # explicit opt-in for http:// (auditor: discouraged)
next_check=1760000000       # managed by the worker
sig_require=no              # require ed25519 signature (generic) / SHA256SUMS.sig
sig_key=/etc/rnsbox/keys/update.pub
```

- **github-releases**: unauthenticated GitHub API (a weekly check is far below
  the rate limit; an optional `token=` key supports private repos), picks the
  newest release (prereleases included when `allow_prerelease=yes` — CI
  publishes auto-builds as prereleases, so this is the default), selects the
  `rnsbox-<board>.img.xz` asset. Checksum source: a `SHA256SUMS` asset if
  present, else the release notes markdown table (`| board | \`file\` |
  \`sha256\` |` — exactly what build.yml emits).
- **generic**: the configured URL returns a JSON manifest
  (schema in `package/rnsbox-update/src/providers/README.md`); covers "other
  sites". HTTPS enforced unless `allow_insecure_http=yes`.
- **Adding a provider = one python module** in `providers/` exposing
  `fetch(cfg) -> Candidate(version, url, sha256)`; registration is a dict entry.

Security rules (auditor-aligned): sha256 is mandatory — a candidate without a
verifiable checksum is discarded; URLs go through `urllib` after
scheme/host validation, never through a shell; all state writes are tmp-file +
rename; the download log is size-capped.

## 3. Schedule — random 7–14 days

After **every** check the worker draws the next interval uniformly from
[7, 14] days and stores `next_check` (epoch) in update.conf. A fresh random
draw per check (not a fixed offset) decorrelates the fleet: routers never
poll in a fixed rhythm and never converge on a cadence an observer could
fingerprint. The hourly cron tick (`S84cron` line) and one best-effort kick at
boot run `rnsbox-sysupdate tick`, which acts only when `now >= next_check`.

**Default: auto-check ON**, provider `github-releases`, repo
`I-AM-ENGINEER/rnsbox`, `allow_prerelease=yes`. Rationale: a router in the
field must get patched (the whole point of the feature), and the A/B rollback
caps the damage of a bad auto-build to "one unconfirmed boot, then revert".
Users who want manual control flip one switch in the web UI.

`allow_prerelease=yes` because push-triggered CI runs — marked prerelease —
are the project's actual distribution channel; full releases are the owner's
manual bless. A bad prerelease costs one rolled-back boot, not a bricked box.

## 4. Transaction (crash-safe by construction)

`rnsbox-sysupdate apply` (flock-guarded, idempotent, resumable-by-state):

1. `download` — `.img.xz` to `/data/rnsbox-update/download.img.xz` (fsync).
2. `verify` — sha256 of the file against the candidate's checksum. Mismatch →
   delete, log, done. Nothing on the device has been touched yet.
3. `write_slot` — read the MBR of the verified image (in RAM), locate the
   *idle* slot partition (current slot from `/boot/ab_slot`, idle = the other
   one), check it exists and is big enough, then stream
   `xz -dc download.img.xz | dd of=/dev/mmcblk0pX bs=4M` (skip/count on the
   decompressed stream; ~20 MB RAM peak on the C906, safe for the 64 MB Duo
   class). fsync. The running slot is never written.
4. `write_boot` — extract p1 boot artifacts from the verified image and refresh
   the shared boot partition (cvitek: `boot.sd` — write `/boot/boot.sd.new`,
   read back, hash-compare, `mv` over `boot.sd`, old copy kept in
   `/data/rnsbox-update/boot-bak`; rpi: same pattern for kernel/dtb/overlays).
   Any mismatch → restore old, abort (slot NOT flipped).
5. `flip` — `/boot/ab_slot=<idle>`, `/boot/ab_tries=0`, pending-state record,
   `sync`, `reboot`.
6. `confirm` — new slot boots; `S98confirm` clears `ab_tries` once rnsd runs
   and the portal listens on :80 (upstream logic, ≤5 min background watcher).
   3 unconfirmed boots → initramfs flips back to the previous slot
   automatically. The worker reconciles the pending-state record on the next
   run → "applied" or "rolled back" in the UI.

Power can vanish at any point: worst case is a corrupt *idle* slot (boot-count
rollback keeps the old slot) or an aborted boot-file refresh with automatic
restore. The device never becomes unbootable by design; the same boot-count
net catches a bad slot written by anything else.

Applicability gate: no A/B layout (`p3` absent) → the UI shows "reflash once
to adopt updates" instead of buttons. Running version unknown (image predates
the version stamp) → auto-apply disabled, manual apply allowed.

## 5. Version stamp

`build.sh` stamps `/etc/rnsbox/image.ver` with the same formula the CI release
job uses for the tag: `r$(date -u +%Y.%m.%d)-${GITHUB_SHA:0:7}` (local builds:
`dev-<date>`). The worker compares candidate tag vs. running stamp
(primary key: the short sha; newer-or-equal date with a different sha counts
as an update). CI-compatibility note: exact match assumes the build and the
release job run in the same UTC day (the general case); a one-line CI tweak —
pass `version` into the build env — would make it exact by construction.

## 6. Portal — "Updates" tab

C++ CGI (`routes_updates.cpp` + `pages/updates.html`, chrome gets an
`updates` sidebar entry). Read path is passive: status JSON is assembled by
the CGI from `/run/rnsbox-sysupdate.json` (last check/apply result, progress
state), update.conf and `/boot/ab_slot`/`ab_tries` — no python spawn, no
network on render. Actions are POST + PRG like every other form:
`/updates/config` (provider/repo/board/enabled/allow_prerelease/https),
`/updates/check`, `/updates/apply`, `/updates/auto`. Each action spawns the
worker detached (`rnsbox-sysupdate ... &` with flock) and redirects; the page
auto-refreshes a status fragment (dashboard-style AJAX JSON), so no CGI
request outlives the uhttpd timeout and the UI never blocks. The last
attempt's log tail (`/data/rnsbox-update/update.log`, capped) is shown on the
page.

## 7. What tests cover

- Host unit tests (`tests/`, python stdlib only): both providers against a
  local fake HTTP server (including the notes-table checksum parse, prerelease
  filtering, manifest schema, sha mismatch rejection, http:// refusal),
  scheduler jitter bounds + next_check persistence, transaction against
  file-backed "disks" (MBR parse, slot range extraction, boot-file staging,
  abort/restore paths), abselect slot logic (ok / rollback / corrupt state).
- QEMU (`riscv64 virt`, harness methodology of `duo01-rv-validate`): the real
  RNSBox rootfs with the package installed; worker runs end-to-end against a
  fake server on 10.0.2.2 and completes a transaction onto file-backed slots;
  abselect decisions exercised in-situ.
- Hardware (user): flash an A/B image, run a real update against a test
  release, then pull power mid-apply and mid-write_boot and verify boot +
  rollback. Checklist in §8.

## 8. Hardware test checklist (for the owner)

1. Flash a current lite image (licheerv-nano or rpi0-2w). Expect `p2 rootfs_A,
   p3 rootfs_B, p4 data` in `fdisk -l` / `/proc/partitions`, `/data` mounted,
   `/boot/ab_slot` = `a`.
2. Portal → Updates: provider fields prefilled, "Check now" finds the newest
   release; "Apply now" downloads, writes slot b, flips, reboots.
3. After reboot: `cat /boot/ab_slot` = `b`, `cat /boot/ab_tries` → 0 within
   ~5 min (S98confirm), portal Dashboard healthy, version shows the new tag.
4. Rollback: apply a deliberately broken update (e.g. point at a manifest with
   a bad image), let it boot 3 times unconfirmed → box comes back on slot a,
   UI shows "rolled back".
5. Power-cut drills: cut power during `write_slot` and during `write_boot`;
   the box must boot the old slot (or the aborted-but-restored old boot files)
   every time.
6. rpi0-2w: same, plus Pi 3/4/5 once each (universal image) — the initramfs
   slot selector is the piece to watch on first real hardware.

## 9. Files (ownership map)

```
package/rnsbox-update/                 # new buildroot package (the agent)
  src/rnsbox-sysupdate                 # python worker (providers, schedule, tx)
  src/providers/{github_releases,generic}.py
  src/abselect                         # POSIX-sh slot logic (boot FAT + /usr/lib)
  src/S90sysupdate, src/S91sysconfirm  # boot kick + post-boot reconcile
  rnsbox-update.mk, Config.in
patches/licheerv-nano/0025-*, 0026-*   # ported A/B layout + boot machinery
board/raspberrypi/rnsbox-0-2w/         # A/B genimage, mini-initramfs, S-scripts
package/rnsbox-portal/src/cgi/         # routes_updates.cpp, update_config in
  routes_*.cpp, pages/updates.html     # store.h/cpp; base.html sidebar entry
build.sh                               # image.ver stamp (+ /data seeding hook)
tests/                                 # unit tests (host, fake HTTP server)
```

Not touched: BLE/RNode/USB portal sections, security-audit fixes, RAM/image
size optimizations.
