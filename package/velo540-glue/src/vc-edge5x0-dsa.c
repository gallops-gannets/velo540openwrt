// SPDX-License-Identifier: GPL-2.0
/* VeloCloud Edge 520/540: hand the two Marvell 88E6176 LAN switches to the
 * mainline DSA driver (mv88e6xxx) without a device tree.
 *
 * Each switch hangs off the SerDes of one I354 function and is reached over
 * that function's own MDIO master, which the patched igb exports as mii_bus
 * "igb-vc-<pci name>" (single-chip addressing, so the switch answers at
 * SMI address 0).  This module creates an mdio_device at address 0 on each of
 * those buses carrying a dsa_mv88e6xxx_pdata whose CPU port (switch port 4)
 * points at the igb netdev of the same function, exactly what
 * mv88e6xxx_probe() expects from platform data.
 *
 *   00:14.0 -> switch A, conduit eth0: port 0 = LAN6, 1 = LAN7, 2 = LAN5, 3 = LAN8
 *   00:14.1 -> switch B, conduit eth1: port 0 = LAN2, 1 = LAN1, 2 = LAN4, 3 = LAN3
 * (jack-to-port map measured by walking a cable round the jacks)
 *
 * Nothing happens unless dsa_mask is set (bit 0 = switch A, bit 1 = switch B),
 * so the plain "two dumb switches" mode of /etc/init.d/velo540-switch stays
 * the default.  With DSA, that script must only bring the CPU-side SerDes up
 * and force switch port 4; DSA owns everything else.
 *
 * Registering both switches needs the DSA-core patch that gives every
 * platform-data switch its own tree (patches/230-*); unpatched kernels put
 * them both in tree 0 and the second one fails to register.
 *
 * Probing is done from a platform driver so that the bus/netdev lookups can
 * return -EPROBE_DEFER until igb has bound.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/mdio.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/dmi.h>
#include <linux/platform_data/mv88e6xxx.h>
#include <net/net_namespace.h>

#define VC_CPU_PORT	4
#define VC_NUM_SW	2
#define VC_LAN_PORTS	4

static unsigned int dsa_mask;
module_param(dsa_mask, uint, 0444);
MODULE_PARM_DESC(dsa_mask, "switches to register with DSA: 1 = 00:14.0 (LAN5-8), 2 = 00:14.1 (LAN1-4), 3 = both; 0 = leave unmanaged (default)");

/* user port names for switch ports 0..3, in port order */
static char *ports_a = "lan6,lan7,lan5,lan8";
static char *ports_b = "lan2,lan1,lan4,lan3";
module_param(ports_a, charp, 0444);
module_param(ports_b, charp, 0444);
MODULE_PARM_DESC(ports_a, "netdev names for switch A (00:14.0) ports 0-3");
MODULE_PARM_DESC(ports_b, "netdev names for switch B (00:14.1) ports 0-3");

struct vc_sw {
	const char *bus_id;
	struct dsa_mv88e6xxx_pdata pdata;
	char names[VC_LAN_PORTS][IFNAMSIZ];
	struct mdio_device *mdiodev;
};

static struct vc_sw vc_sw[VC_NUM_SW] = {
	{ .bus_id = "igb-vc-0000:00:14.0" },
	{ .bus_id = "igb-vc-0000:00:14.1" },
};

/* mdio_device_bus_match() is not exported to modules: same thing, locally */
static int vc_mdio_bus_match(struct device *dev, const struct device_driver *drv)
{
	struct mdio_device *mdiodev = to_mdio_device(dev);
	const struct mdio_driver *mdiodrv = to_mdio_driver(drv);

	if (mdiodrv->mdiodrv.flags & MDIO_DEVICE_IS_PHY)
		return 0;
	return strcmp(mdiodev->modalias, drv->name) == 0;
}

/* the igb netdev of the PCI function that owns this MDIO bus */
static struct net_device *vc_conduit_for(struct device *parent)
{
	struct net_device *nd, *found = NULL;

	rtnl_lock();
	for_each_netdev(&init_net, nd) {
		if (nd->dev.parent == parent) {
			dev_hold(nd);
			found = nd;
			break;
		}
	}
	rtnl_unlock();
	return found;
}

static int vc_sw_register(struct vc_sw *sw, const char *names)
{
	struct mdio_device *md;
	struct net_device *nd;
	struct mii_bus *bus;
	char buf[64], *p, *tok;
	int i, err;

	bus = mdio_find_bus(sw->bus_id);
	if (!bus)
		return -EPROBE_DEFER;

	nd = vc_conduit_for(bus->parent);
	if (!nd) {
		err = -EPROBE_DEFER;
		goto err_bus;
	}

	memset(&sw->pdata, 0, sizeof(sw->pdata));
	strscpy(buf, names, sizeof(buf));
	p = buf;
	for (i = 0; i < VC_LAN_PORTS; i++) {
		tok = strsep(&p, ",");
		if (!tok || !*tok) {
			pr_err("vc-edge5x0-dsa: bad port name list \"%s\"\n", names);
			err = -EINVAL;
			goto err_nd;
		}
		strscpy(sw->names[i], tok, IFNAMSIZ);
		sw->pdata.cd.port_names[i] = sw->names[i];
	}
	sw->pdata.cd.port_names[VC_CPU_PORT] = (char *)"cpu";
	sw->pdata.compatible = "marvell,mv88e6085";	/* 88E6352 family */
	sw->pdata.enabled_ports = GENMASK(VC_CPU_PORT, 0);
	sw->pdata.netdev = nd;		/* mv88e6xxx dev_put()s it on remove */
	sw->pdata.eeprom_len = 0;
	sw->pdata.irq = 0;		/* no INTn wired: poll the PHYs */

	md = mdio_device_create(bus, 0);
	if (IS_ERR(md)) {
		err = PTR_ERR(md);
		goto err_nd;
	}
	strscpy(md->modalias, "mv88e6085", sizeof(md->modalias));
	md->bus_match = vc_mdio_bus_match;	/* match on modalias (mdio_device_create leaves it NULL) */
	md->dev.platform_data = &sw->pdata;

	err = mdio_device_register(md);
	if (err) {
		mdio_device_free(md);
		goto err_nd;
	}
	sw->mdiodev = md;
	put_device(&bus->dev);
	pr_info("vc-edge5x0-dsa: 88E6176 on %s -> DSA, conduit %s, ports %s\n",
		sw->bus_id, netdev_name(nd), names);
	return 0;

err_nd:
	dev_put(nd);
err_bus:
	put_device(&bus->dev);
	return err;
}

static void vc_sw_unregister(struct vc_sw *sw)
{
	struct mdio_device *md = sw->mdiodev;
	bool bound;

	if (!md)
		return;
	bound = md->dev.driver != NULL;
	mdio_device_remove(md);
	/* mv88e6xxx releases the conduit reference itself once it has probed */
	if (!bound)
		dev_put(sw->pdata.netdev);
	mdio_device_free(md);
	sw->mdiodev = NULL;
}

static int vc_dsa_probe(struct platform_device *pdev)
{
	int i, err;

	for (i = 0; i < VC_NUM_SW; i++) {
		if (!(dsa_mask & BIT(i)) || vc_sw[i].mdiodev)
			continue;
		err = vc_sw_register(&vc_sw[i], i ? ports_b : ports_a);
		if (err)
			return err;	/* -EPROBE_DEFER until igb is bound */
	}
	return 0;
}

static void vc_dsa_remove(struct platform_device *pdev)
{
	int i;

	for (i = VC_NUM_SW - 1; i >= 0; i--)
		vc_sw_unregister(&vc_sw[i]);
}

static struct platform_driver vc_dsa_driver = {
	.probe = vc_dsa_probe,
	.remove_new = vc_dsa_remove,
	.driver = {
		.name = "vc-edge5x0-dsa",
	},
};

static struct platform_device *vc_dsa_pdev;

static int __init vc_dsa_init(void)
{
	const char *board = dmi_get_system_info(DMI_BOARD_NAME);
	int err;

	if (!board || (strcmp(board, "EDGE520") && strcmp(board, "EDGE540")))
		return -ENODEV;
	if (!dsa_mask) {
		pr_info("vc-edge5x0-dsa: dsa_mask=0, switches left unmanaged\n");
		return 0;
	}

	err = platform_driver_register(&vc_dsa_driver);
	if (err)
		return err;
	vc_dsa_pdev = platform_device_register_simple("vc-edge5x0-dsa", -1, NULL, 0);
	if (IS_ERR(vc_dsa_pdev)) {
		platform_driver_unregister(&vc_dsa_driver);
		return PTR_ERR(vc_dsa_pdev);
	}
	return 0;
}

static void __exit vc_dsa_exit(void)
{
	if (!dsa_mask)
		return;
	platform_device_unregister(vc_dsa_pdev);
	platform_driver_unregister(&vc_dsa_driver);
}

module_init(vc_dsa_init);
module_exit(vc_dsa_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VeloCloud Edge 5x0 88E6176 switches as DSA (platform data)");
