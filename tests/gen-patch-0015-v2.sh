#!/bin/bash
# gen-patch-0015-v2.sh — regenerate the arm64 wifi patch with the correct
# per-package handling:
#   sg2000_duo01evb (BGA): &wifisd reference works (label intact in the
#     base dtsi; soph_asic_bga.dtsi does not delete it) + wifi_pin
#     porta15/porte7 (same as the RV sg2000_duomodule01_sd board).
#   sg2002_duo (QFN): soph_asic_qfn.dtsi DELETEs wifi-sd@4320000 and ships
#     its own wifi_pin (porte2/porte6). Re-declare the FULL node in the
#     board dts (exactly what the vendor sg2002_licheea53nano arm board
#     does after the same delete) and do NOT touch wifi_pin.
set -eu
REPO=/home/noname/init-fix
WS=/home/noname/rnsbox/milkv-duo-01-arm
SCRATCH=/tmp/rnsbox-scratch2
export GIT_AUTHOR_NAME=rnsbox GIT_AUTHOR_EMAIL=rnsbox@localhost
export GIT_COMMITTER_NAME=rnsbox GIT_COMMITTER_EMAIL=rnsbox@localhost
export GIT_AUTHOR_DATE="2026-10-17T00:30:00+03:00"
export GIT_COMMITTER_DATE="$GIT_AUTHOR_DATE"

D1=build/boards/sg200x/sg2000_duo01evb_arm64_sd/dts_arm64/sg2000_duo01evb_arm64_sd.dts
D2=build/boards/sg200x/sg2002_duo_arm64_sd/dts_arm64/sg2002_duo_arm64_sd.dts
K1=build/boards/sg200x/sg2000_duo01evb_arm64_sd/linux/sg2000_duo01evb_arm64_sd_defconfig
K2=build/boards/sg200x/sg2002_duo_arm64_sd/linux/sg2002_duo_arm64_sd_defconfig

rm -rf "$SCRATCH"; mkdir -p "$SCRATCH/tree"
git -C "$SCRATCH/tree" init -q
git -C "$SCRATCH/tree" config user.name rnsbox
git -C "$SCRATCH/tree" config user.email rnsbox@localhost
for f in "$D1" "$D2" "$K1" "$K2"; do
    mkdir -p "$SCRATCH/tree/$(dirname "$f")"
    git -C "$WS" show "HEAD:$f" > "$SCRATCH/tree/$f"   # pristine HEAD state
    chmod --reference="$WS/$f" "$SCRATCH/tree/$f"      # keep the real mode
done
git -C "$SCRATCH/tree" add -A && git -C "$SCRATCH/tree" commit -qm base

# ---- sg2000 (BGA): reference the intact label, add the wifi_pin node ----
python3 - "$SCRATCH/tree/$D1" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
block = '''
// Duo Module 01-W only: AIC8800 over SDIO (the -W module carries the
// radio; the plain Module 01 has none and the absence stays soft —
// S35wifi exits 0 when no wlanN appears, the portal shows "No WiFi
// radio detected"). The base dtsi only stubs the label with the
// interrupt line: these are the node properties that make the cvitek
// sdio driver actually probe (same ones the vendor arm boards carry).
// Power/wakeup pins per the Milk-V Duo S reference design; the
// cvi_wifi_pin driver (CONFIG_CVI_WIFI_PIN below) owns the WL_REG_ON /
// WL_HOST_WAKE sequencing for the aic8800_bsp probe.
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
'''
s = s.rstrip("\n") + "\n" + block
assert "wifisd" in s
open(p, "w").write(s)
print("sg2000 dts: wifisd + wifi_pin appended")
PYEOF

# ---- sg2002 (QFN): the qfn dtsi deletes the node — re-create it in full ----
python3 - "$SCRATCH/tree/$D2" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
block = '''
// Duo (SG2002) -W radio only: AIC8800 over SDIO. soph_asic_qfn.dtsi
// DELETES wifi-sd@4320000 (the vendor default for QFN carriers without a
// radio), so the node is re-declared here IN FULL — the same thing the
// vendor sg2002_licheea53nano arm board does after the same delete. The
// qfn dtsi's own wifi_pin (poweron porte2 / wakeup porte6) stays: it is
// the vendor wiring for this package. On carriers without the -W radio
// the absence stays soft (S35wifi exits 0, portal shows "No WiFi radio
// detected").
/ {
	wifisd:wifi-sd@4320000 {
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
};
'''
s = s.rstrip("\n") + "\n" + block
open(p, "w").write(s)
print("sg2002 dts: full wifisd node re-declared")
PYEOF

# ---- both kernel defconfigs: CVI_WIFI_PIN ----
python3 - "$SCRATCH/tree/$K1" "$SCRATCH/tree/$K2" <<'PYEOF'
import sys
for p in (sys.argv[1], sys.argv[2]):
    s = open(p).read()
    anchor = "CONFIG_MMC_SDHCI_CVI=y\n"
    assert anchor in s, "MMC anchor not found in %s" % p
    assert "CONFIG_CVI_WIFI_PIN" not in s, "already in %s" % p
    add = anchor + "# RNSBox: the cvitek,wifi-pin DT driver — WL_REG_ON/HOST_WAKE\n" \
        + "# sequencing for the AIC8800 SDIO radio of the -W boards\n" \
        + "CONFIG_CVI_WIFI_PIN=y\n"
    open(p, "w").write(s.replace(anchor, add, 1))
    print("CVI_WIFI_PIN ->", p)
PYEOF

git -C "$SCRATCH/tree" add -A
git -C "$SCRATCH/tree" commit -qm "wifi: AIC8800 SDIO on the arm64 board family

The arm64 board dts only inherited the wifisd LABEL STUB from
soph_base_arm.dtsi (interrupt line, no compatible/reg/clocks), so the
cvitek sdio driver never probed the AIC8800 of the -W boards and the
portal showed \"No WiFi radio detected\" on boards that have the radio.
Per package:
- sg2000_duo01evb (BGA): fill the &wifisd node (same properties as the
  vendor arm boards carry) and add the wifi_pin node (porta15/porte7,
  the Milk-V Duo S reference wiring — same as the RV duo01 board).
- sg2002_duo (QFN): soph_asic_qfn.dtsi deletes wifi-sd@4320000 outright;
  re-declare the full node (what the vendor sg2002_licheea53nano board
  does after the same delete) and keep the qfn wifi_pin (porte2/porte6).
Both linux defconfigs gain CONFIG_CVI_WIFI_PIN for the cvitek,wifi-pin
driver (WL_REG_ON / WL_HOST_WAKE sequencing for aic8800_bsp). The plain
(non-W) boards have no radio: S35wifi already degrades to a logged
no-op, so the absence stays soft."
git -C "$SCRATCH/tree" format-patch -1 --stdout > "$REPO/patches/milkv-duo-01-arm/0015-wifi-aic8800-sdio-on-the-arm64-board-family.patch"
echo "regenerated milkv-duo-01-arm/0015"
