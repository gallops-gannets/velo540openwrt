// SPDX-License-Identifier: GPL-2.0
/* VeloCloud Edge 520/540: register the GPIO bit-bang MDIO bus that carries
 * the two Marvell 88E1514 WAN PHYs, using mainline mdio-gpio + gpio-ich.
 *
 * Pins (from the vendor velocloud-vc.c), SoC core-well GPIO numbers, which
 * are offsets 0..31 on the gpio-ich "gpio_ich" chip (Avoton layout):
 *   rev A (DMI board version 1.x):  MDIO = 11, MDC = 12
 *   rev B (DMI board version 2.x, platform "edge540b"): MDIO = 13, MDC = 14
 *
 * Builds as an out-of-tree module (OpenWrt kmod package); no kernel patch.
 * Load order: gpio-ich (built in) -> this -> mdio-gpio -> igb (igb defers
 * funcs 2/3 with -EPROBE_DEFER until the bus "gpio-0" exists).
 *
 * If gpiod_get fails with -ENODEV the pins are not muxed as GPIO in
 * GPIO_USE_SEL (coreboot did not do it; the vendor gpio-pcu driver did).
 *
 * Tested on an EDGE540 rev 2.8 with OpenWrt 25.12.5.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/machine.h>
#include <linux/dmi.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/property.h>
#include <linux/err.h>

/* Atom C2000 LPC bridge: GPIO base address register and core-well
 * GPIO_USE_SEL.  coreboot leaves the MDC/MDIO pins in native-function
 * mode; gpio-ich refuses to hand out a line whose USE_SEL bit is clear.
 * The register only tolerates 32-bit accesses.
 */
#define C2000_LPC_GBASE		0x48
#define C2000_GPIO_USE_SEL	0x00

static int vc_gpio_use_sel(u32 bits)
{
	struct pci_dev *lpc;
	u32 gbase, v;

	lpc = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0x1f, 0));
	if (!lpc)
		return -ENODEV;
	pci_read_config_dword(lpc, C2000_LPC_GBASE, &gbase);
	pci_dev_put(lpc);
	gbase &= 0xff80;
	if (!gbase)
		return -ENODEV;

	v = inl(gbase + C2000_GPIO_USE_SEL);
	if ((v & bits) != bits) {
		outl(v | bits, gbase + C2000_GPIO_USE_SEL);
		pr_info("vc-edge5x0-mdio: GPIO_USE_SEL @%#x: %#x -> %#x\n",
			gbase, v, inl(gbase + C2000_GPIO_USE_SEL));
	}
	return 0;
}

static struct gpiod_lookup_table vc_mdio_gpios = {
	.dev_id = "mdio-gpio.0",
	.table = {
		/* index 0 = MDC, index 1 = MDIO (mdio-gpio.c) */
		GPIO_LOOKUP_IDX("gpio_ich", 14, NULL, 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("gpio_ich", 13, NULL, 1, GPIO_ACTIVE_HIGH),
		{ },
	},
};

static struct platform_device *vc_mdio_pdev;

/* Board-control I2C bus (rev B: "SMB1" pins bit-banged, SoC core GPIO 11 = SDA,
 * 12 = SCL) carrying the two PCA9557 expanders: 0x18 = PCIe/switch/PHY resets,
 * RF kill, USB power; 0x1c = PoE control.  Registered as adapter i2c-9 for
 * mainline i2c-gpio; userspace instantiates the pca9557 (see
 * /etc/init.d/velo540-switch).
 */
#define VC_I2C_BUS_NR 9
static struct gpiod_lookup_table vc_i2c_gpios = {
	.dev_id = "i2c-gpio.9",
	.table = {
		GPIO_LOOKUP_IDX("gpio_ich", 11, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN), /* sda */
		GPIO_LOOKUP_IDX("gpio_ich", 12, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN), /* scl */
		{ },
	},
};
static struct platform_device *vc_i2c_pdev;

/* Front logo RGB LED: PCA9634 at 0x54 on the same bus, channels 0/1/2 = R/G/B,
 * totem-pole outputs, inverted (LEDs are active low), output enable on SoC
 * GPIO 39 (gpio-ich line 551, active low; the init script drives it).
 * Described to the mainline leds-pca963x driver with software nodes, so the
 * three colours show up as /sys/class/leds/pca963x:{red,green,blue}:logo.
 */
static const struct property_entry vc_led_props[] = {
	PROPERTY_ENTRY_BOOL("nxp,totem-pole"),
	PROPERTY_ENTRY_BOOL("nxp,inverted-out"),
	{ }
};
static const struct software_node vc_led_node = { .name = "pca9634", .properties = vc_led_props };
#define VC_LED(n, idx, lbl, trig) \
	static const struct property_entry vc_led_##n##_props[] = { \
		PROPERTY_ENTRY_U32("reg", idx), PROPERTY_ENTRY_STRING("label", lbl), \
		PROPERTY_ENTRY_STRING("linux,default-trigger", trig), { } }; \
	static const struct software_node vc_led_##n##_node = { \
		.name = "led-" #idx, .parent = &vc_led_node, .properties = vc_led_##n##_props }
VC_LED(red, 0, "red:logo", "none");
VC_LED(green, 1, "green:logo", "default-on");
VC_LED(blue, 2, "blue:logo", "none");
static const struct software_node *vc_led_nodes[] = {
	&vc_led_node, &vc_led_red_node, &vc_led_green_node, &vc_led_blue_node, NULL
};
static struct i2c_board_info vc_led_info = {
	I2C_BOARD_INFO("pca9634", 0x54),
	.swnode = &vc_led_node,
};
static struct i2c_client *vc_led_client;

/* i2c_register_board_info() is init-only; create the client once the
 * i2c-gpio adapter exists (deferred probe until then). */
static int vc_led_probe(struct platform_device *pdev)
{
	struct i2c_adapter *adap = i2c_get_adapter(VC_I2C_BUS_NR);

	if (!adap)
		return -EPROBE_DEFER;
	vc_led_client = i2c_new_client_device(adap, &vc_led_info);
	i2c_put_adapter(adap);
	if (IS_ERR(vc_led_client)) {
		int err = PTR_ERR(vc_led_client);

		vc_led_client = NULL;
		return err;
	}
	return 0;
}
static void vc_led_remove(struct platform_device *pdev)
{
	if (vc_led_client)
		i2c_unregister_device(vc_led_client);
	vc_led_client = NULL;
}
static struct platform_driver vc_led_driver = {
	.probe = vc_led_probe,
	.remove_new = vc_led_remove,
	.driver = { .name = "vc-edge5x0-led" },
};
static struct platform_device *vc_led_pdev;

static int __init vc_mdio_init(void)
{
	const char *board = dmi_get_system_info(DMI_BOARD_NAME);
	const char *ver = dmi_get_system_info(DMI_BOARD_VERSION);

	if (!board || (strcmp(board, "EDGE520") && strcmp(board, "EDGE540")))
		return -ENODEV;

	if (!ver || ver[0] != '2') {		/* rev A pinout */
		vc_mdio_gpios.table[0].chip_hwnum = 12;
		vc_mdio_gpios.table[1].chip_hwnum = 11;
	}

	vc_gpio_use_sel(BIT(vc_mdio_gpios.table[0].chip_hwnum) |
			BIT(vc_mdio_gpios.table[1].chip_hwnum) |
			BIT(vc_i2c_gpios.table[0].chip_hwnum) |
			BIT(vc_i2c_gpios.table[1].chip_hwnum));

	gpiod_add_lookup_table(&vc_i2c_gpios);
	vc_i2c_pdev = platform_device_register_simple("i2c-gpio", VC_I2C_BUS_NR, NULL, 0);
	if (IS_ERR(vc_i2c_pdev))
		pr_warn("vc-edge5x0-mdio: i2c-gpio registration failed: %ld\n", PTR_ERR(vc_i2c_pdev));
	/* logo LED: PCA9634 client on that adapter, described by software nodes */
	if (software_node_register_node_group(vc_led_nodes) == 0 &&
	    platform_driver_register(&vc_led_driver) == 0) {
		vc_led_pdev = platform_device_register_simple("vc-edge5x0-led", -1, NULL, 0);
		if (IS_ERR(vc_led_pdev)) {
			pr_warn("vc-edge5x0-mdio: logo LED device registration failed\n");
			vc_led_pdev = NULL;
		}
	}

	gpiod_add_lookup_table(&vc_mdio_gpios);
	vc_mdio_pdev = platform_device_register_simple("mdio-gpio", 0, NULL, 0);
	if (IS_ERR(vc_mdio_pdev)) {
		gpiod_remove_lookup_table(&vc_mdio_gpios);
		return PTR_ERR(vc_mdio_pdev);
	}
	pr_info("vc-edge5x0-mdio: %s rev %s, WAN PHY MDIO bus on gpio_ich %u/%u\n",
		board, ver ? ver : "?", vc_mdio_gpios.table[0].chip_hwnum,
		vc_mdio_gpios.table[1].chip_hwnum);
	return 0;
}

static void __exit vc_mdio_exit(void)
{
	if (vc_led_pdev)
		platform_device_unregister(vc_led_pdev);
	platform_driver_unregister(&vc_led_driver);
	software_node_unregister_node_group(vc_led_nodes);
	platform_device_unregister(vc_mdio_pdev);
	gpiod_remove_lookup_table(&vc_mdio_gpios);
	if (!IS_ERR_OR_NULL(vc_i2c_pdev))
		platform_device_unregister(vc_i2c_pdev);
	gpiod_remove_lookup_table(&vc_i2c_gpios);
}

module_init(vc_mdio_init);
module_exit(vc_mdio_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VeloCloud Edge 5x0 WAN PHY MDIO bus glue");
