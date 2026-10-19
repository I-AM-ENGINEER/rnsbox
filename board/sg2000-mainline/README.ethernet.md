# Ethernet on the Duo Module 01 EVB (mainline 7.3)

The EVB's 100M RJ45 is driven by the **SG2000 internal EPHY** through the
internal RMII path.  There is no external Ethernet PHY and no Ethernet
driver for the `TMI7003C`: that chip is the board's 3-channel DCDC + LDO
power regulator (VIN/SW/FB pins, EN straps to VSYS), not a PHY.

`linux-patches/0001-*` enables both the MAC and the MDIO mux:

    &gmac0 { status = "okay"; };
    &mdio  { status = "okay"; };

`linux.config` carries `CONFIG_STMMAC_ETH`, `CONFIG_DWMAC_GENERIC`,
`CONFIG_DWMAC_SOPHGO`, `CONFIG_PHYLIB` and `CONFIG_MDIO_BUS_MUX_MMIOREG`.

Verified live on the EVB, kernel 7.3.0-rc7:

    stmmaceth 4070000.ethernet: Active PHY interface: RMII (4)
    stmmaceth 4070000.ethernet eth0: PHY [mdio_mux-0.0:01] driver [Generic PHY]
    stmmaceth 4070000.ethernet eth0: configuring for phy/internal link mode
    stmmaceth 4070000.ethernet eth0: Link is Up - 100Mbps/Full - flow control rx/tx
    eth0: inet 192.168.88.207/24

The LicheeRV Nano (SG2002) uses the same EPHY; its missing `gmac0`/`mdio`
enablement is in `board/sg2002-mainline/linux-patches/`.
