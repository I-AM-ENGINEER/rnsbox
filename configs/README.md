# Board configuration: shared base + per-board deltas

```
configs/base.config                 selections shared by every external board
configs/boards/<board>.config       board delta only
configs/rnsbox_<board>_defconfig    generated file (committed, never edited)
scripts/gen-defconfigs.sh           assembles the defconfigs; --check verifies
```

`configs/rnsbox_<board>_defconfig` is `base.config` followed by
`boards/<board>.config`. The delta wins: if it mentions a symbol (with a value
or `# ... is not set`), the generator drops that symbol's line from the base,
so the generated file never contains duplicate assignments.

The generated defconfigs are committed on purpose, so plain
`make rnsbox_<board>_defconfig` keeps working and Buildroot's download cache
keys stay stable. `scripts/gen-defconfigs.sh --check` fails when the base or a
delta changed without regenerating; both `build.sh` (external flow) and the
GitHub Actions workflows run it.

Every board — including the vendor-fip CVITEK ones — uses this same external
rootfs defconfig layer; there is no second buildroot tree to keep in sync.

## Change the shared set (e.g. add a package)

1. Add the line to `configs/base.config` if every board needs it.
2. Regenerate:

   ```sh
   scripts/gen-defconfigs.sh
   ```

3. Commit `base.config` and the changed `configs/rnsbox_*_defconfig` files
   together. The generated diff is mechanical; review the base.

If only some boards need the package, put it in their
`configs/boards/<board>.config` instead.

## Add a board

1. Create `configs/boards/<board>.config`: copy a similar board's delta and
   keep only the differences (architecture, toolchain, kernel source/DTS,
   overlay, rootfs size, WiFi/BT, users table). The shared router/portal
   stack comes from `base.config`.
2. Add `DEFCONFIG=rnsbox_<board>_defconfig` and a branch in `build.sh`, plus a
   matrix entry in CI.
3. Generate the defconfig and verify it:

   ```sh
   scripts/gen-defconfigs.sh
   scripts/gen-defconfigs.sh --check
   ```

## Override or disable a shared symbol

Use the delta:

```
# this board has no zsh
# BR2_PACKAGE_ZSH is not set

# different rootfs size
BR2_TARGET_ROOTFS_EXT2_SIZE="256M"
```

The generator removes the base line for that symbol; only the delta's value
remains in the generated defconfig.

## Don't

- Don't edit `configs/rnsbox_<board>_defconfig` by hand: `gen-defconfigs.sh`
  overwrites it, and `--check` reports the mismatch.
- Don't repeat a base line in a delta unless it is an intentional override.
- Don't put board-varying settings in the base (root password, users table,
  post-build scripts, overlay, image size, WiFi/BT).
- Don't use `make savedefconfig` to edit a board config: it minimizes a full
  `.config` and knows nothing about the base/delta split. Edit the delta (or
  the base) and regenerate.
