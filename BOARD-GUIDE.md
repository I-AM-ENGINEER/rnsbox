# Adding a board to RNSBox

This repository is a [Buildroot **external tree**][br-ext] (`external.desc`
at the root). One repo, many boards, no patch series and no per-board
forks: shared router software lives once under `package/`, every board is a
defconfig + a board directory, and every board builds into its own output
directory against the same pinned upstream.

[br-ext]: https://buildroot.org/downloads/manual/manual.html#outside-br-custom

## Layout

```
external.desc / external.mk / Config.in   external-tree registration
configs/<board>_defconfig                 one per board
board/<vendor>/<board>/                   kernel fragments, overlays, gadget
                                          scripts, genimage config…
package/                                  SHARED packages (portal, python-rns,
                                          python-lxmf, python-nomadnet…)
boards/<board>.sh                         build.sh entry: upstream pin + style
scripts/build-variant.sh                  lite/dvd packaging (any board)
patches/<board>/                          ONLY for patch-style boards whose
                                          upstream is a vendor BSP fork
```

Two board styles coexist:

- **`BUILD_STYLE=external`** (preferred; e.g. `rpi0-2w`) — upstream is
  official Buildroot at a pinned commit, cloned **once and shared by every
  external board** (`~/rnsbox-work/upstream/buildroot-<shortsha>`), each
  board building with `O=<upstream>/out/<board>` and a shared download
  cache. You never patch upstream; anything upstream lacks becomes an
  external package or a board file.
- **`BUILD_STYLE=patches`** (legacy; `licheerv-nano`) — upstream is a vendor
  BSP fork, and the delta is a git-am patch series under `patches/<board>/`.
  Vendor-coupled boards stay here until their stack can be expressed as
  external packages.

## Adding an external-style board

1. **Board dir** — `board/<vendor>/<board>/`:
   - `linux.fragment` (kernel config delta), firmware/boot files if the
     board needs them (e.g. `config.txt`, `cmdline.txt`),
   - `rootfs-overlay/` — init scripts, `/etc/rnsbox/*` configs. Copy the
     shape from `board/raspberrypi/rnsbox-0-2w/rootfs-overlay/` and adapt
     the hardware specifics (gadget composer, WiFi driver calls, UART
     nodes). Keep board differences in *configs*, not in code forks.
   - `post-build.sh` / `post-image.sh` (optional; reference them with
     `$(BR2_EXTERNAL_RNSBOX_PATH)/board/...` paths in the defconfig).
2. **Defconfig** — `configs/rnsbox_<board>_defconfig`. Start from the
   rpi0-2w one; all RNSBox paths must use the `$(BR2_EXTERNAL_RNSBOX_PATH)`
   prefix. Shared package options (`BR2_PACKAGE_RNSBOX_PORTAL`,
   `BR2_PACKAGE_PYTHON_RNS`, …) are board-independent.
3. **Board entry** — `boards/<board>.sh`:

   ```sh
   BUILD_STYLE=external
   UPSTREAM_URL="https://github.com/buildroot/buildroot.git"
   UPSTREAM_COMMIT="<pinned buildroot commit>"
   DEFCONFIG=rnsbox_<board>_defconfig
   rnsbox_build() { make -C "$UPSTREAM" O="$ODIR" BR2_EXTERNAL="$EXT" "$DEFCONFIG"
                    "$EXT/scripts/build-variant.sh" "$VARIANT" "$UPSTREAM" "$ODIR" "$EXT"; }
   ```

4. Build: `./build.sh <board> lite` (or `dvd`). Add the board to the
   `matrix.include` list in `.github/workflows/build.yml` and CI builds it
   alongside the rest.

Rules of thumb:

- A new package goes to `package/<name>/` once and is instantly available
  to **every** board — never copy a package into a board dir.
- Never redefine a package that upstream Buildroot already ships
  (`make package/<name>` exists upstream ⇒ use theirs, patch via
  `BR2_GLOBAL_PATCH_DIR` if a fix is truly needed).
- Scripts in this repo are LF-only (`.gitattributes` enforces `*.sh`).

## Adding a patch-style (vendor BSP) board

Only when the board's kernel/boot flow exists solely inside a vendor
buildroot fork: pin the fork in `boards/<board>.sh` (`UPSTREAM_URL`,
`UPSTREAM_COMMIT`, `BUILD_STYLE=patches` default) and add the git-format
series under `patches/<board>/`. Regenerate the series with
`git format-patch` whenever the tree changes. Long-term, migrate the board
to the external style by moving its delta into `board/` + `configs/` and
keeping only vendor bits (kernel, bootloader sources) referenced from the
defconfig.
