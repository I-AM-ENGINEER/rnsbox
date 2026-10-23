/*
 * mt7628_esw - minimal MT7628 built-in switch initialisation.
 *
 * Mainline Linux has no driver for the MT7628 ESW, so after the bootloader
 * hands over the switch is left in a state where broadcast/multicast reach the
 * CPU but unicast does not (no working VLAN/CPU-port configuration).
 *
 * This driver reproduces the register sequence of OpenWrt's esw_rt3050.c and
 * configures a single flat VLAN 0: all present ports untagged, PVID 0, the
 * CPU port a member.  The board then works with a plain eth0 (no eth0.1).
 *
 * Port map for HT-HD01-V2: port 0 = RJ45, port 2 = SR9900, port 6 = CPU.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/delay.h>

#define ESW_FCT0		0x08
#define ESW_PFC1		0x14
#define ESW_PVIDC(n)		(0x40 + 4 * (n))
#define ESW_VLANI(n)		(0x50 + 4 * (n))
#define ESW_FPA			0x84
#define ESW_SOCPC		0x8c
#define ESW_POC0		0x90
#define ESW_POC2		0x98
#define ESW_SGC			0x9c
#define ESW_P0LED		0xa4	/* P0..P4 at 0xa4 + 4*n */
#define ESW_FPA2		0xc8
#define ESW_FCT2		0xcc
#define ESW_SGC2		0xe4

#define PORTS_ALL		0x7f
#define PORTS_NOCPU		0x3f
#define PORT_DISABLE_DEFAULT	0x3a
#define PORTMAP_DEFAULT		0x07

#define LED_LINKACT		5

static int mt7628_esw_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	void __iomem *base;
	u32 port_map = PORTMAP_DEFAULT;
	u32 port_disable = PORT_DISABLE_DEFAULT;
	int i;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	of_property_read_u32(np, "mediatek,portmap", &port_map);
	of_property_read_u32(np, "mediatek,portdisable", &port_disable);

	/*
	 * esw_hw_init() from OpenWrt's esw_rt3050.c.  VLANs are enabled for
	 * every port, all ports untagged (POC2=0x7f7f includes the CPU port,
	 * so plain eth0 works instead of OpenWrt's tagged eth0.1).
	 */
	writel(0xC8A07850, base + ESW_FCT0);
	writel(0x00000000, base + ESW_SGC2);

	/* port priority 1 for all ports, VLAN enabled for all ports */
	writel(0x00005555 | (PORTS_ALL << 16), base + ESW_PFC1);

	/* back pressure + flow control on all ports */
	writel(PORTS_ALL | (PORTS_ALL << 8), base + ESW_POC0);

	/* aging on, VLAN tag removal on all ports (CPU too) */
	writel((PORTS_ALL << 8) | PORTS_ALL, base + ESW_POC2);

	writel(0x0002500c, base + ESW_FCT2);
	writel(0x0008a301, base + ESW_SGC);

	/* allow unknown/multicast/broadcast to the CPU port */
	writel(0x02000000 | (0x40 << 0) | (0x40 << 8) | (0x40 << 16),
	       base + ESW_SOCPC);

	writel(0x3f502b28, base + ESW_FPA2);
	writel(0x00000000, base + ESW_FPA);

	for (i = 0; i < 5; i++)
		writel(LED_LINKACT, base + ESW_P0LED + 4 * i);

	/* flat VLAN 0: every port PVID 0, VLAN 0 id 0 */
	for (i = 0; i < 4; i++)
		writel(0, base + ESW_PVIDC(i));
	writel(0, base + ESW_VLANI(0));

	/* LAN port map, then disable the nonexistent PHYs (1,3,4,5) */
	writel((port_map & 0x3f) << 24, base + ESW_SGC2);
	writel((PORTS_ALL | (PORTS_ALL << 8)) |
	       ((port_disable & 0x7f) << 23), base + ESW_POC0);

	dev_info(&pdev->dev,
		 "MT7628 ESW initialised: portmap=0x%02x portdisable=0x%02x, flat VLAN 0\n",
		 port_map, port_disable);

	return 0;
}

static const struct of_device_id mt7628_esw_of_ids[] = {
	{ .compatible = "mediatek,mt7628-esw" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt7628_esw_of_ids);

static struct platform_driver mt7628_esw_driver = {
	.probe = mt7628_esw_probe,
	.driver = {
		.name = "mt7628-esw",
		.of_match_table = mt7628_esw_of_ids,
	},
};
module_platform_driver(mt7628_esw_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal MT7628 built-in switch initialisation");
