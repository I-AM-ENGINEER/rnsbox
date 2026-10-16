#!/bin/bash
# gen-patches2.sh — arm64-series patches: WiFi (wifisd DT node + CVI_WIFI_PIN)
# and the existence-checked loadsystemko (kills the soph_*.ko insmod noise).
set -eu

REPO=/home/noname/init-fix
WS=/home/noname/rnsbox/milkv-duo-01-arm
SCRATCH=/tmp/rnsbox-scratch2
export GIT_AUTHOR_NAME=rnsbox GIT_AUTHOR_EMAIL=rnsbox@localhost
export GIT_COMMITTER_NAME=rnsbox GIT_COMMITTER_EMAIL=rnsbox@localhost
export GIT_AUTHOR_DATE="2026-10-17T00:30:00+03:00"
export GIT_COMMITTER_DATE="$GIT_AUTHOR_DATE"

seed() {
    mkdir -p "$SCRATCH/tree/$(dirname "$1")"
    cp "$WS/$1" "$SCRATCH/tree/$1"
}
commit() {
    git -C "$SCRATCH/tree" add -A
    git -C "$SCRATCH/tree" commit -qm "$1

$2"
}
out_patch() {
    git -C "$SCRATCH/tree" format-patch -1 --stdout > "$1"
}

rm -rf "$SCRATCH"; mkdir -p "$SCRATCH/tree"
git -C "$SCRATCH/tree" init -q
git -C "$SCRATCH/tree" config user.name rnsbox
git -C "$SCRATCH/tree" config user.email rnsbox@localhost

D1=build/boards/sg200x/sg2000_duo01evb_arm64_sd/dts_arm64/sg2000_duo01evb_arm64_sd.dts
D2=build/boards/sg200x/sg2002_duo_arm64_sd/dts_arm64/sg2002_duo_arm64_sd.dts
K1=build/boards/sg200x/sg2000_duo01evb_arm64_sd/linux/sg2000_duo01evb_arm64_sd_defconfig
K2=build/boards/sg200x/sg2002_duo_arm64_sd/linux/sg2002_duo_arm64_sd_defconfig
for f in "$D1" "$D2" "$K1" "$K2"; do seed "$f"; done
git -C "$SCRATCH/tree" add -A && git -C "$SCRATCH/tree" commit -qm base

WIFI_DTS_BLOCK='
// Duo Module 01-W only: AIC8800 over SDIO (the -W module carries the
// radio; the plain Module 01 has none and the absence stays soft —
// S35wifi exits 0 when no wlanN appears, the portal shows "No WiFi
// radio detected"). Full node (the base dtsi only stubs the label with
// the interrupt): same properties the vendor sg2002_licheea53nano_sd
// arm board carries. Power/wakeup pins per the Milk-V Duo S reference
// design; the cvi_wifi_pin driver (CONFIG_CVI_WIFI_PIN below) owns the
// WL_REG_ON / WL_HOST_WAKE sequencing for the aic8800_bsp probe.
&wifisd {
	compatible = "cvitek,cv181x-sdio";
	bus-width = <4>;
	reg = <0x0 0x4320000 0x0 0x1000>;
	reg_names = "core_mem";
	src-frequency = <375000000>;
	min-frequency = <400000>; // 400Khz
	max-frequency = <25000000>; // 25Mhz
	64_addressing;
	reset_tx_rx_phy;
	non-removable;
	pll_index = <0x7>;
	pll_reg = <0x300207C>;
	no-mmc;
	no-sd;
	status = "okay";
	interrupts = <GIC_SPI 22 IRQ_TYPE_LEVEL_HIGH>;
};

/ {
	wifi_pin {
		compatible = "cvitek,wifi-pin";
		poweron-gpio = <&porta 15 GPIO_ACTIVE_HIGH>;
		wakeup-gpio = <&porte 7 GPIO_ACTIVE_HIGH>;
	};
};
'

python3 - "$SCRATCH/tree/$D1" "$SCRATCH/tree/$D2" "$WIFI_DTS_BLOCK" <<'PYEOF'
import sys
block = sys.argv[3]
anchor = """&sd {
	min-frequency = <400000>; // 400Khz
	max-frequency = <25000000>; // 25Mhz
};
"""
for p in (sys.argv[1], sys.argv[2]):
    s = open(p).read()
    assert anchor in s, "sd cap block not found in %s" % p
    assert "wifisd" not in s, "wifisd already present in %s" % p
    open(p, "w").write(s + block)
    print("wifi dts block ->", p)
PYEOF

python3 - "$SCRATCH/tree/$K1" "$SCRATCH/tree/$K2" <<'PYEOF'
import sys
for p in (sys.argv[1], sys.argv[2]):
    s = open(p).read()
    anchor = "CONFIG_MMC_SDHCI_CVI=y\n"
    assert anchor in s, "MMC anchor not found in %s" % p
    assert "CONFIG_CVI_WIFI_PIN" not in s, "CVI_WIFI_PIN already in %s" % p
    add = anchor + "# RNSBox: the cvitek,wifi-pin DT driver — WL_REG_ON/HOST_WAKE\n" \
        + "# sequencing for the AIC8800 SDIO radio of the Duo Module 01-W\n" \
        + "CONFIG_CVI_WIFI_PIN=y\n"
    s = s.replace(anchor, add, 1)
    open(p, "w").write(s)
    print("CVI_WIFI_PIN ->", p)
PYEOF

commit "wifi: AIC8800 SDIO on the arm64 board family" \
"The arm64 board dts only inherited the wifisd LABEL STUB from
soph_base_arm.dtsi (interrupt line, no compatible/reg/clocks), so the
cvitek sdio driver never probed the AIC8800 of the -W modules and the
portal showed \"No WiFi radio detected\" on boards that have the radio.
Add the full node (same properties as the vendor sg2002_licheea53nano
arm board) plus the wifi_pin power/wakeup node and CONFIG_CVI_WIFI_PIN.
The plain (non-W) Module 01 has no radio: S35wifi already degrades to a
logged no-op, so the absence stays soft."
out_patch "$REPO/patches/milkv-duo-01-arm/0015-wifi-aic8800-sdio-on-the-arm64-board-family.patch"
echo "wrote milkv-duo-01-arm/0015 (wifi)"

############################################################################
# Patch 0016: existence-checked loadsystemko (no more soph_* insmod noise)
############################################################################
F=ramdisk/rootfs/overlay/64bit/system/ko/loadsystemko.sh
seed "$F"
git -C "$SCRATCH/tree" add -A && git -C "$SCRATCH/tree" commit -qm base
python3 - "$SCRATCH/tree/$F" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
start = s.index("insmod /mnt/system/ko/soph_sys.ko")
end = s.index("insmod /mnt/system/ko/soph_ive.ko")
end = s.index("\n", end) + 1
old_block = s[start:end]
lines = old_block.splitlines()
new_lines = [
    "# RNSBox: the headless image does not carry the multimedia/vendor ko",
    "# subset (the slimmer and the ko stash only keep what something loads),",
    "# so insmod every soph_* module ONLY when its file is on the image —",
    "# the unconditional insmods spammed one \"can't insert ... No such",
    "# file\" line per missing module into every boot log.",
]
for ln in lines:
    ln = ln.strip()
    if not ln or ln.startswith("#"):
        continue
    assert ln.startswith("insmod /mnt/system/ko/"), "unexpected line: %s" % ln
    args = ln[len("insmod /mnt/system/ko/"):].split()
    ko = args[0]
    rest = " ".join(args[1:])
    new_lines.append('[ -f /mnt/system/ko/%s ] && insmod /mnt/system/ko/%s %s' % (ko, ko, rest))
new_block = "\n".join(new_lines) + "\n"
s = s.replace(old_block, new_block, 1)
open(p, "w").write(s)
print("loadsystemko rewritten")
PYEOF
commit "ramdisk: insmod only the soph kos the image actually carries" \
"The headless rootfs ships only the kos something loads; the vendor
loadsystemko.sh still insmod'd the full multimedia list unconditionally,
so every boot logged one \"insmod: can't insert 'soph_*.ko': No such
file\" per absent module. Existence-check each module line instead —
same order, same parameters, silent for anything not shipped."
out_patch "$REPO/patches/milkv-duo-01-arm/0016-ramdisk-insmod-only-the-soph-kos-the-image-carries.patch"
echo "wrote milkv-duo-01-arm/0016 (loadsystemko)"

echo "PHASE2-DONE"
