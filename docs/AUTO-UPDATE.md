# RNSBox auto-update: design and user guide

Status: implemented on branch `auto-update` (base: `raspberry-pi-zero-2w`).
Upstream groundwork: `upstream/auto-updater` (`7fa2cfb`, HW-validated A/B boot
machinery on the LicheeRV Nano) — Phase 1+2 ported as series patches 0025–0027,
Phase 3 (the actual update agent) written here as the `rnsbox-update` buildroot
package.

## 1. Update model — A/B whole-OS image (chosen)

The device keeps two complete rootfs partitions (rootfs_A = p2, rootfs_B =
p3) plus a shared boot partition (p1) and a persistent data partition (p4).
An update writes the *idle* slot from the downloaded whole-OS image, then
flips a slot pointer. The boot chain picks the active slot and auto-reverts
after 3 unconfirmed boots (boot-count in `/boot/ab_tries`, cleared by
S98confirm once rnsd runs and the portal listens on :80).

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
  (and the dvd variant) are detected (`no p3`) and the portal shows "flash a
  current image once, then updates work over the air".
- Image raw size roughly doubles (two 1 GiB slots for licheerv-nano, two
  512 MiB for rpi0-2w); the published `.img.xz` grows only by a second copy
  of the OS content (empty slot space is zeros → xz), i.e. roughly 2x asset
  size.
- SD card: **≥ 4 GiB recommended** for licheerv-nano, ≥ 1.5 GiB for rpi0-2w.
- dvd variant: incompatible with fixed slots (the clients ISO does not fit)
  — `scripts/build-variant.sh` rejects it until slot sizes are raised on
  purpose.
- Shared-boot residual risk: p1 (boot.sd on cvitek, kernel/dtb/firmware on
  rpi) is refreshed in place AFTER the new slot is fully written and
  verified but BEFORE the slot flip. A power cut inside this seconds-wide
  window can damage boot files; the old copies are kept on /data
  (`boot-bak/`) and restored automatically on mismatch, and the slot is not
  flipped on failure — the box reboots into the old slot either way. This
  matches the risk level the upstream HW-validated flow already accepts for
  `boot.sd`.

### What is ported vs. new

| Piece | Where |
|---|---|
| 4-partition genimage (`genimage_rootless_ab.cfg`), S03data (persistent /data + state bind-mounts + 256 MB swap), S99resizefs A/B mode, build-rnsbox.sh lite A/B swap | series patch `0025` (port of upstream `3e6fe92` patch 0023) |
| initramfs slot selector + boot-count rollback (AB_MAX=3) + unmountable-slot hard fallback, S98confirm health confirm, timing fix `7fa2cfb` | series patch `0026` (port of upstream patch 0024 + fix), extended: the decision logic is SOURCED from `/boot/rnsbox-abselect` (single source, unit/QEMU-testable) with the validated upstream logic as the inline fallback for a missing/corrupt lib |
| cron wiring + defconfig | series patch `0027` |
| providers, 7–14 day jitter scheduler, download/verify/apply transaction, `/etc/rnsbox/update.conf`, portal "Updates" tab, image version stamp | new: `package/rnsbox-update`, `routes_updates.cpp`, `build.sh` |
| RPi A/B layout + mini-initramfs (busybox via gen_init_cpio), rpi S03data/S98confirm, A/B-aware S02resizefs, dvd rejection | `board/raspberrypi/rnsbox-0-2w/` (needs real-Pi boot validation) |

## 2. Providers

Config lives in `/etc/rnsbox/update.conf` (flat `key=value`, read/written by
the CGI `store` and the python worker):

```
enabled=yes                 # auto-check master switch (default yes, see §3)
provider=github-releases    # github-releases | generic
repo=I-AM-ENGINEER/rnsbox   # github-releases: owner/repo
url=                        # generic: full URL of manifest.json
board=auto                  # auto | rpi0-2w | licheerv-nano (asset selector)
token=                      # optional (private repos / rate limits)
allow_prerelease=yes        # see §3
allow_insecure_http=no      # explicit opt-in for http:// (auditor: discouraged)
auto_apply=no               # apply without a confirmation click
interval_min_days=7
interval_max_days=14
next_check=0                # managed by the worker
sig_require=no              # require ed25519 signature (generic provider)
sig_key=/etc/rnsbox/keys/update.pub
```

- **github-releases**: unauthenticated GitHub API (a weekly check is far
  below the rate limit), picks the newest release — prereleases included
  when `allow_prerelease=yes` — and selects `rnsbox-<board>.img.xz`.
  Checksum source: a `SHA256SUMS` asset if present, else the release notes
  markdown table (`| board | asset | sha256 |` — exactly what build.yml
  emits).
- **generic**: the configured URL returns a JSON manifest (schema in
  `package/rnsbox-update/src/update_providers/README.md`); file URLs may be
  relative. Covers "other sites". HTTPS enforced unless
  `allow_insecure_http=yes`. Optional ed25519 manifest signature
  (`sig_require=yes`, key pinned in the image, python-cryptography).
- **Adding a provider = one python module** in
  `package/rnsbox-update/src/update_providers/` exposing
  `fetch(conf, board, fetch_url, truthy)`; import name = config name with
  dashes replaced by underscores.

Security rules: sha256 is mandatory — a candidate without a verifiable
checksum is discarded; HTTPS enforced unless explicitly allowed (redirects
re-checked); URLs never touch a shell (urllib only); all state writes are
tmp-file + fsync + rename; the on-device log is size-capped; portal actions
run the worker via execve-argv (no shell) and are POST-only with same-origin
enforced by the portal.

## 3. Schedule — random 7–14 days

After **every** check the worker draws the next interval uniformly from
[min, max] days (default 7–14) and stores `next_check` (epoch) in
update.conf. A fresh random draw per check decorrelates the fleet: no fixed
rhythm, no converging cadence to fingerprint. The hourly cron tick
(`43 * * * *`, added by S84cron) and one best-effort kick at boot
(S90sysupdate) run `rnsbox-sysupdate tick`, which acts only when
`now >= next_check`.

**Default: auto-check ON**, provider `github-releases`, repo
`I-AM-ENGINEER/rnsbox`, `allow_prerelease=yes`, `auto_apply=no`.
Rationale: a router in the field must get patched, and the A/B rollback caps
the damage of a bad auto-build to "one unconfirmed boot, then revert".
Auto-*apply* stays manual by default: a whole-OS swap unattended is riskier
than a check, and the portal makes confirming it one click. Users who want
full manual control flip the master switch in the web UI.

`allow_prerelease=yes` because push-triggered CI runs — marked prerelease —
are the project's actual distribution channel; full releases are the owner's
manual bless.

## 4. Transaction (crash-safe by construction)

`rnsbox-sysupdate apply` (flock-guarded, resumable via the state file):

1. `download` — `.img.xz` to `<state>/download.img.xz` (fsync; reuses a
   verified download).
2. `verify` — sha256 of the file against the candidate checksum. Mismatch →
   delete, log, error. Nothing on the device touched yet.
3. `write_slot` — parse the MBR from the decompressed stream, locate the
   *idle* slot (p2/p3 from `/boot/ab_slot`), check it exists and is large
   enough, then stream `xz -dc | dd` with exact byte accounting (skip to the
   partition offset first; ~20 MB RAM peak, safe for 64 MB-RAM boards).
   fsync. The running slot is never written.
4. `write_boot` — extract p1 boot artifacts from the verified image
   (cvitek whitelist: `boot.sd` + `fip.bin`, CIMG header stripped if
   present; rpi: everything except per-card state and config files), keep
   old copies in `<state>/boot-bak/`, write `.new` + read-back hash check +
   rename. Any mismatch → restore, abort (slot NOT flipped).
5. `flip` — `/boot/ab_slot=<idle>`, `/boot/ab_tries=0`, fsync, record the
   pending state, `reboot`.
6. `confirm` — new slot boots; S98confirm clears `ab_tries` once healthy
   (≤5 min watcher). 3 unconfirmed boots → the initramfs flips back. The
   worker reconciles the pending record on the next tick → "applied" or
   "rolled back" in the UI.

Power can vanish at any point: the worst case is a corrupt *idle* slot (the
boot-count rollback keeps the old one) or an aborted boot-file refresh with
automatic restore. The device never becomes unbootable by design.

## 5. Version stamp

`build.sh` exports `RNSBOX_BUILD_VER` with the same formula the CI release
job uses for the tag: `r$(date -u +%Y.%m.%d)-${GITHUB_SHA:0:7}` (local
builds: `dev-<date>`), and the package installs it as
`/etc/rnsbox/image.ver`. The worker compares the candidate tag against the
running stamp (primary key: the short sha; a different sha with a >= build
date counts as newer). An unknown running version disables auto-apply;
manual apply stays available. CI-compatibility note: the stamp matches the
release tag when the build and the release job run in the same UTC day (the
normal case); a one-line CI tweak — passing `version` into the build env —
would make it exact by construction.

## 6. Portal — "Updates" tab

`routes_updates.cpp` + `pages/updates.html` (sidebar entry in base.html).
Read path is passive: status comes from `/run/rnsbox-sysupdate.json`,
update.conf and the boot slot files — no python spawn, no network on
render. Actions are POST + PRG: `/updates/config`, `/updates/check`,
`/updates/apply`, `/updates/auto`. Each spawns the worker via util::run
(execve argv — no shell) and redirects; the worker flock-guards and
double-forks, so nothing blocks; the page JS polls `/updates/data` while a
phase is in flight. The last attempt's log tail (`update.log`, capped) is
shown on the page.

## 7. Tests

- Host unit tests (`tests/test_sysupdate.py`, stdlib only, 17 tests):
  both providers against a local fake HTTP server (notes-table checksum
  parse, prerelease filtering, missing-sha refusal, manifest schema,
  http:// refusal), version comparison, full transaction against
  file-backed "disks" (MBR parse, slot range extraction, CIMG strip, boot
  backup, flip, pending state), sha-mismatch abort, reconcile
  applied/rolled-back, abselect (first boot / rollback / corrupt state),
  FAT LFN reading against mkfs.vfat-made filesystems.
- QEMU (`tests/qemu/run-rv.sh`, methodology of `duo01-rv-validate`): boots
  a real RNSBox rootfs (package injected via debugfs) on the riscv64 virt
  kernel; the worker runs end-to-end against a fake server on 10.0.2.2:
  status, check, the full transaction onto file-backed slots, content
  hashes, flip, boot-file staging + backup, reconcile applied/rolled-back,
  abselect rollback, https enforcement, running-slot cleanliness.
- Hardware (owner): see §8.

## 8. Hardware test checklist (for the owner)

1. Flash a current lite image (licheerv-nano or rpi0-2w). Expect four
   partitions (`fdisk -l`: boot, rootfs_A, rootfs_B, data), `/data` mounted,
   `/boot/ab_slot` = `a`.
2. Portal → Updates: fields prefilled, "Check now" finds the newest release,
   "Apply now" downloads, writes the idle slot, flips, reboots.
3. After reboot: `/boot/ab_slot` = `b`, `/boot/ab_tries` → 0 within ~5 min
   (S98confirm), portal Dashboard healthy, running version shows the new
   tag, UI shows "applied".
4. Rollback: apply a deliberately broken image (e.g. a generic manifest
   pointing at garbage with a valid sha), let it boot 3 times unconfirmed →
   the box comes back on the old slot, UI shows "rolled back".
5. Power-cut drills: cut power during `writing_slot` and during
   `writing_boot`; the box must boot the old slot every time.
6. rpi0-2w: same, plus Pi 3/4/5 once each — the mini-initramfs slot selector
   is the piece to watch on first real hardware (the cvitek path is the
   HW-validated reference).

## 9. CI compatibility

- build.yml publishes ONE prerelease per run with `rnsbox-<board>.img.xz`
  assets and checksums quoted in the notes table — that exact shape is what
  the github-releases provider consumes (SHA256SUMS assets supported too,
  preferred when present).
- Asset size: the A/B layout roughly doubles the published `.img.xz`
  (two OS copies; empty slot space compresses to nothing).
- The lite variant is the CI default and IS the A/B layout (build-rnsbox.sh
  swaps the genimage cfg; the committed BSP tree stays dvd-shaped).
- `image.ver` uses the release-tag formula; exactness across UTC-midnight
  builds would need a one-line CI change (pass `version` through).
- CIMG-wrapped boot.sd assets (if any other channel produces them) are
  detected and stripped during boot-file staging.

## 10. Files (ownership map)

```
package/rnsbox-update/                 # the agent (worker, providers,
  src/rnsbox-sysupdate                 # abselect, S90sysupdate, conf)
  src/providers/…  (update_providers/)
package/rnsbox-portal/src/cgi/         # Updates tab
  routes_updates.cpp, pages/updates.html, store.{h,cpp} (UpdateConfig),
  routes.h, main.cpp, render.cpp, pages/base.html
patches/licheerv-nano/0025–0027-*.patch  # A/B layout + boot machinery + wiring
board/raspberrypi/rnsbox-0-2w/         # A/B genimage, mini-initramfs,
                                       # S02resizefs/S03data/S98confirm
build.sh                               # RNSBOX_BUILD_VER stamp
scripts/build-variant.sh               # dvd rejected under A/B
configs/rnsbox_rpi0_2w_64_defconfig    # RNSBOX_UPDATE=y + CA_CERTIFICATES=y
tests/                                 # host unit tests + QEMU acceptance
```

Not touched: BLE/RNode/USB portal sections, security-audit fixes, RAM/image
size optimizations.
