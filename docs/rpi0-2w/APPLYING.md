# (moved from the former patch series; build mechanics are now BR2_EXTERNAL — see BOARD-GUIDE.md; content otherwise as shipped)

# Applying and building RNSBox (Raspberry Pi Zero 2 W)

RNSBox ships as a `git format-patch` series on top of upstream Buildroot. You
apply the series onto a pristine upstream checkout and run the RNSBox build
driver.

## 1. Get the upstream base

```
git clone https://github.com/buildroot/buildroot.git
cd buildroot
git checkout 679b9ead7620bbf193620d1ebf56f53c1764d37a   # 2026.02.3
```

The series was generated against exactly this commit.

## 2. Apply the RNSBox patch series

```
git am /path/to/rnsbox-patches/*.patch
```

If you would rather not create commits, `git apply` also works:

```
git apply /path/to/rnsbox-patches/*.patch
```

## 3. Build

A plain `make` with the RNSBox defconfig builds the base system; the driver
selects the image variant:

```
make rnsbox_rpi0_2w_64_defconfig
make                                     # toolchain + kernel + rootfs + sdcard.img
./build-rnsbox.sh lite                   # NCM-only image (~400 MB, default)
./build-rnsbox.sh dvd                    # + read-only clients disc (auto-sized)
```

Notes:

- Build host: a Linux machine set up for Buildroot (Ubuntu with
  `build-essential git wget cpio unzip rsync bc file bzip2 zstd perl python3
  jq libncurses-dev dosfstools` is enough; add `xorriso` for the dvd
  variant). In WSL, strip the Windows interop PATH entries first (see
  boards/rpi0-2w.sh in the rnsbox repo).
- The `rns` (Reticulum) version is the latest release at build time when
  building through `rnsbox build.sh`; a bare `make` uses the pinned fallback
  in `package/python-rns`.
- The `dvd` variant expects the Reticulum client binaries under `dl/clients/`.
  Run `./fetch-clients.sh` first and review `clients-stick/` — see the
  follow-up note in [README.RNSBox.md](README.RNSBox.md). The `lite` variant
  needs none of this.
- Output images land under `output/images/`, suffixed `-dvd` / `-lite`.

## 4. Flash

```
sudo dd if=output/images/rnsbox-rpi0-2w-<variant>.img \
        of=/dev/sdX bs=4M conv=fsync status=progress
```

The DVD image needs a microSD comfortably larger than the clients disc
(e.g. 8 GB); the rootfs auto-grows to fill the card on first boot.

Then plug the board's micro-USB port into a host and browse to
`http://10.42.0.1/` (admin / admin).
