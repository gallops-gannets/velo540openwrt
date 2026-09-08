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
 * UNTESTED DRAFT.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/machine.h>
#include <linux/dmi.h>

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
	platform_device_unregister(vc_mdio_pdev);
	gpiod_remove_lookup_table(&vc_mdio_gpios);
}

module_init(vc_mdio_init);
module_exit(vc_mdio_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VeloCloud Edge 5x0 WAN PHY MDIO bus glue");
