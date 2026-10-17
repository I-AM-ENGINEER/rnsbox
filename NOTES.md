# NOTES — ram-rnsd-fixes (2026-10-17)

Two tasks, one root cause shared between them: the board was shipped with
silently broken plumbing — 170 MB of DRAM reserved for a media pipeline that
never runs, and a Python stdlib extension without which `rnsd` could not
create a single TCP socket.

---

## 1. RAM: ~311 MB -> expected ~457 MB free on the 512 MB board

The vendor `memmap.py` reserved a 170 MB ION carveout for the CVITEK
ISP/H26x media pipeline. On this headless Reticulum router the pipeline is
never instantiated — the carveout was pure waste (measured live:
`MemTotal: 318180 kB`, i.e. ~311 MB of a nominal 512 MB).

**Change:** `ION_SIZE = 170 * SIZE_1M` -> `ION_SIZE = 24 * SIZE_1M`
(BSP patch `patches/milkv-duo-01-arm/0017-duo01evb-arm64-shrink-the-ION-carveout-170M-24M-headless.patch`).

| region         | before              | after               |
|----------------|---------------------|---------------------|
| DRAM           | 512 MB @ 0x80000000 | 512 MB @ 0x80000000 |
| OPENSBI + ATF  | 512K + 512K @ 0     | unchanged           |
| UIMAG (fsbl/uboot load buffer) | 16 MB @ +24 MB | unchanged (conservative) |
| BOOTLOGO / FRAMEBUFFER | 1800K      | unchanged (already cut) |
| **ION**        | **170 MB** @ +338.2 MB | **24 MB** @ +486 MB |
| H26X bitstream / ISP base (inside ION) | 2 MB + 20 MB | unchanged, still fit the assert (2+20 <= 24) |
| FREERTOS (C906L) | 2 MB @ +510 MB   | unchanged           |
| KERNEL_MEMORY  | 510 MB              | unchanged (ION is reserved-memory *inside* it) |

Conservative by design: ONE line changed; UIMAG/FREERTOS/BOOTLOGO untouched
because fip/fsbl/uboot all regenerate `cvi_board_memmap.h` from this single
file (`build/scripts/mmap.mk` has a proper make dependency), so the whole
boot chain stays self-consistent — u-boot is rebuilt from the same memmap.
24 MB instead of 0 keeps the `ion-region` reserved-memory node real: a
zero-size region would break the cvitek ion driver's
`of_reserved_mem_device_init` path, and the assert
`ISP_MEM_BASE_ADDR + ISP_MEM_BASE_SIZE <= ION_ADDR + ION_SIZE` must hold.

**Expected after flash:** `dmesg | grep -i memory:` / `free -m` shows
MemTotal ~457 MB (311 + 146 reclaimed). assert-image now enforces
DTB memory node >= 440 MB and ion carveout <= 32 MB.

## 2. rnsd dying silently — ROOT CAUSE FOUND AND FIXED (not just diagnostics)

The task brief asked for diagnostics (python -u, death recorder). The live
board gave up the actual root cause instead; the fix is a config knob.

### The failure chain (all steps verified live over SSH, admin@192.168.88.230)

1. syslog: `echobot[837] ... rnsd's shared instance is up (waited 11s)` then
   `LookupError: unknown encoding: idna` from
   `RNS/Interfaces/TCPInterface.py -> socket.getaddrinfo`.
2. `/var/log/rnsd.log` == 0 bytes: the LookupError escapes Reticulum as an
   unhandled exception and RNS exits via `os._exit` — nothing flushes, no
   traceback lands in the daemon's own log.
3. wanwatch log: `clock-synced but rnsd not running -> kicking S82rnsd`
   every 10 s, `Starting rnsd: OK (pid ...)` each time — a crash-loop.
4. After `down_limit=3600` s of that, wanwatch reboots the board: the
   hourly reboot cycle the owner saw (`/run/rnsd-down-since` = 706 @
   uptime 707).
5. OOM ruled out: nothing oom/killed in the kernel log path.

### Root cause

`cvitek_SG200X_64bit_defconfig` (and `cvitek_SG200X_musl_riscv64_defconfig`)
shipped with `BR2_PACKAGE_PYTHON3_UNICODEDATA` unset -> buildroot configured
CPython `--disable-unicodedata` -> the target had NO
`usr/lib/python3.11/lib-dynload/unicodedata*.so`. `encodings.idna` imports
`from unicodedata import ucd_3_2_0` at codec-load time, so **every**
`socket.getaddrinfo()` on the board raised
`LookupError: unknown encoding: idna` — even `getaddrinfo("0.0.0.0", ...)`
(reproduced as admin with a one-liner). rnsd creates its TCPServerInterface
(`listen_ip = 0.0.0.0` in the shipped `/etc/reticulum/config`) right after
start -> death within seconds of every start.

The "rnsd lives when run from admin" observation was an artifact: admin's
rnsd ATTACHED to the shared instance as a customer (root's rnsd was being
re-kicked every 10 s, so a server was usually alive mid-death-loop), and a
customer never synthesizes interfaces. Proven live: a stub `unicodedata.py`
on `PYTHONPATH` makes `import encodings.idna` succeed and getaddrinfo
proceed; the real C extension provides everything idna needs
(`stringprep`/`_stringprep` were already present — only `unicodedata` was
missing).

**Fix:** `BR2_PACKAGE_PYTHON3_UNICODEDATA=y` in both 64bit defconfigs
(BSP patch `patches/milkv-duo-01-arm/0018-buildroot-enable-python-unicodedata-on-the-64bit-configs.patch`,
cost ~1.1 MB rootfs), enforced as a build.sh policy knob for every cvitek
defconfig (`RNSBOX_POLICY_KNOBS` + live-.config stamp + a python3 dirclean
when a warm target predates the flip). The 32bit defconfig always had the
knob on — the 64bit ones simply never did, so this broke both the arm64
boards AND the rv64 images the same way.

### Diagnostics still added (for the next mystery — BSP patch 0034)

- `S82rnsd` launches rnsd with `PYTHONUNBUFFERED=1` (the env spelling of
  `python -u`, shebang-agnostic): rnsd's stdout log lines land in
  `/var/log/rnsd.log` as printed instead of dying in a block buffer.
- `early_death_watchdog` in `S82rnsd`: a root-side 60 s watcher after every
  start; if the pid dies within the minute (and it was not a deliberate
  stop nor a portal restart) it writes `/var/log/rnsd-death` with
  timestamp, uptime, rnsd.log tail and a 40-line dmesg tail — root CAN read
  dmesg, the admin SSH account cannot, which is half of why this stayed a
  mystery.

## 3. /tmp was 0755 root (admin could not write it) — fixed

Not an rcS issue: the buildroot skeleton symlinks `/var/log`,
`/var/cache`, `/var/spool`, `/var/tmp` -> `../tmp`, so the old per-dir
`/var/*` tmpfs fstab entries never made separate mounts — `mount(8)`
followed the symlinks and stacked THREE anonymous tmpfs mounts onto /tmp
(observed live: `size=4096k,mode=755` then `size=512k,mode=755` on top).
The last one won: /tmp was a 512 KB 0755 layer shared with /var/log. The
`/var/lock` entry never applied at all (`../run/lock` is created by
inittab si4 AFTER `mount -a`).

**Fix** (BSP patch 0035): one `tmpfs /tmp ... rw,nosuid,nodev,mode=1777,size=8M`
entry; all /var symlinks land on it. assert-image enforces the shape
(single 1777 /tmp entry, no /var/* tmpfs lines).

## Files touched

- BSP series (applied to the warm workspace `~/rnsbox/milkv-duo-01-arm`,
  committed there as 4 commits, exported verbatim):
  - `patches/milkv-duo-01-arm/0017-...` — ION 170M -> 24M
  - `patches/milkv-duo-01-arm/0018-...` — UNICODEDATA=y (both 64bit defconfigs)
  - `patches/licheerv-nano/0034-...` — S82rnsd unbuffered + death recorder
  - `patches/licheerv-nano/0035-...` — fstab single 1777 /tmp tmpfs
- repo infra: `build.sh` (policy knob + stamp + conditional python3
  dirclean), `board/common/assert-image.sh` (unicodedata/idna guard, fstab
  shape guard, DTB RAM >= 440 MB + ion <= 32 MB assertions via optional
  3rd argument).

## What the owner does next

1. Flash `~/ramfix-artifacts/rnsbox-sg2000-arm64-lite.img` (the fresh build
   from this branch; assert-image green).
2. Send back:
   - `dmesg | grep -iE 'memory|Memory:'` and `free -m` — expect MemTotal
     ~457 MB (not 311);
   - `cat /var/log/rnsd.log` — with PYTHONUNBUFFERED the full RNS startup
     log should now appear; rnsd should be STABLE (the idna crash is gone);
   - if anything still dies within a minute: `cat /var/log/rnsd-death`
     (timestamp + log tail + dmesg tail, captured by the new watchdog);
   - `ls -ld /tmp` — expect drwxrwxrwt (1777), 8 MB tmpfs.
3. The wanwatch hourly reboot cycle stops on its own once rnsd stays up
   (down-streak never accumulates to down_limit=3600).

## Build notes

- Built from `~/ramfix` with `WORKDIR=$HOME/rnsbox/milkv-duo-01-arm
  RNSBOX_NPROC=8 ./build.sh sg2000-arm64 lite` under
  `flock ~/rnsbox/.build.lock`; the warm workspace carried the merged
  series + these 4 commits (no reset).
- python3 was dircleaned once so `--enable-unicodedata` actually rebuilds
  into a warm target (build.sh now does this automatically when needed).
- assert-image run: see the build log / final report.
