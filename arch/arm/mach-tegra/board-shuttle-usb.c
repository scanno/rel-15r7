/*
 * arch/arm/mach-tegra/board-shuttle-usb.c
 *
 * Copyright (C) 2011 Eduardo José Tagle <ejtagle@tutopia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/* All configurations related to USB */

/* Misc notes on USB bus on Tegra2 systems (extracted from several conversations
held regarding USB devices not being recognized)

  For additional power saving, the tegra ehci USB driver supports powering
down the phy on bus suspend when it is used, for example, to connect an 
internal device that use an out-of-band remote wakeup mechanism (e.g. a 
gpio).

  With power_down_on_bus_suspend = 1, the board fails to recognize newly
attached devices, i.e. only devices connected during boot are accessible.
But it doesn't cause problems with the devices themselves. The main cause
seems to be that power_down_on_bus_suspend = 1 will stop the USB ehci clock
, so we dont get hotplug events.

  Seems that it is needed to keep the IP clocked even if phy is powered 
down on bus suspend, since otherwise we don't get hotplug events for
hub-less systems.

*/
 
#include <linux/kobject.h>
#include <linux/console.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/platform_data/tegra_usb.h>
#include <linux/gpio.h>
#include <linux/mfd/tps6586x.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/i2c-tegra.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/time.h>
#include <asm/setup.h>

#include <mach/io.h>
#include <mach/iomap.h>
#include <mach/irqs.h>
#include <mach/gpio.h>
#include <mach/clk.h>
#include <mach/usb_phy.h>
#include <mach/system.h>

#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>
#include <linux/usb/f_accessory.h>

#include "board.h"
#include "board-shuttle.h"
#include "clock.h"
#include "gpio-names.h"
#include "devices.h"


#ifdef CONFIG_USB_SUPPORT
static struct tegra_usb_platform_data tegra_udc_pdata = {
	.port_otg = true,
	.has_hostpc = false,
	.phy_intf = TEGRA_USB_PHY_INTF_UTMI,
	.op_mode = TEGRA_USB_OPMODE_DEVICE,
	.u_data.dev = {
		.vbus_pmu_irq = 0,
		.vbus_gpio = -1,
		.charging_supported = false,
		.remote_wakeup_supported = false,
	},
	.u_cfg.utmi = {
		.hssync_start_delay = 0,
		.idle_wait_delay 	= 17,
		.elastic_limit 		= 16,
		.term_range_adj 	= 6, 	/*  xcvr_setup = 9 with term_range_adj = 6 gives the maximum guard around */
		.xcvr_setup 		= 9, 	/*  the USB electrical spec. This is true across fast and slow chips, high */
									/*  and low voltage and hot and cold temperatures */
		.xcvr_lsfslew 		= 2,	/*  -> To slow rise and fall times in low speed eye diagrams in host mode */
		.xcvr_lsrslew 		= 2,	/*                                                                        */
		.xcvr_setup_offset = 0,
		.xcvr_use_fuses = 1,
	},
};

static struct tegra_usb_platform_data tegra_ehci1_utmi_pdata = {
	.port_otg = true,
	.has_hostpc = false,
	.phy_intf = TEGRA_USB_PHY_INTF_UTMI,
	.op_mode = TEGRA_USB_OPMODE_HOST,
	.u_data.host = {
		.vbus_gpio = -1,
		.vbus_reg = NULL,
		.hot_plug = false,
		.remote_wakeup_supported = false,
		.power_off_on_suspend = false,
	},
	.u_cfg.utmi = {
		.hssync_start_delay = 0,
		.idle_wait_delay 	= 17,
		.elastic_limit 		= 16,
		.term_range_adj 	= 6, 	/*  xcvr_setup = 9 with term_range_adj = 6 gives the maximum guard around */
		.xcvr_setup 		= 9, 	/*  the USB electrical spec. This is true across fast and slow chips, high */
									/*  and low voltage and hot and cold temperatures */
		.xcvr_lsfslew 		= 2,	/*  -> To slow rise and fall times in low speed eye diagrams in host mode */
		.xcvr_lsrslew 		= 2,	/*                                                                        */
	},
};

static struct tegra_usb_platform_data tegra_ehci3_utmi_pdata = {
	.port_otg = false,
	.has_hostpc = false,
	.phy_intf = TEGRA_USB_PHY_INTF_UTMI,
	.op_mode	= TEGRA_USB_OPMODE_HOST,
	.u_data.host = {
		.vbus_gpio = -1,
		.vbus_reg = NULL,
		.hot_plug = true,
		.remote_wakeup_supported = false,
		.power_off_on_suspend = false,
	},
	.u_cfg.utmi = {
		.hssync_start_delay = 0,
		.idle_wait_delay 	= 17,
		.elastic_limit 		= 16,
		.term_range_adj 	= 6,	/*  -> xcvr_setup = 9 with term_range_adj = 6 gives the maximum guard around */
		.xcvr_setup 		= 9,	/*     the USB electrical spec. This is true across fast and slow chips, high */
									/*     and low voltage and hot and cold temperatures */
		.xcvr_lsfslew 		= 2,	/*  -> To slow rise and fall times in low speed eye diagrams in host mode */
		.xcvr_lsrslew 		= 2,	/*                                                                        */
	},
};

static struct tegra_usb_otg_data tegra_otg_pdata = {
	.ehci_device = &tegra_ehci1_device,
	.ehci_pdata = &tegra_ehci1_utmi_pdata,
};

static struct platform_device *shuttle_usb_devices[] __initdata = {
	/* OTG should be the first to be registered */
	&tegra_otg_device,
	&tegra_udc_device, 		/* USB gadget */
	&tegra_ehci3_device,
};

static bool in_host_mode = false;
static void tegra_set_host_mode(void)
{
	pr_debug("Switching to host mode\n");
	
	if (in_host_mode)
		return;
	in_host_mode = true;
	
	/* Place interface in host mode	*/

	/* Switch to host mode */
	tegra_otg_set_host_mode(true);

	/* Enable VBUS - This means we can power USB devices, but
	   we cant use VBUS detection at all */
	gpio_direction_input(SHUTTLE_USB0_VBUS);

	/* Leave some time for stabilization purposes */
	msleep(10);

}

static void tegra_set_gadget_mode(void)
{
	pr_debug("Switching to gadget mode\n");
	
	if (!in_host_mode)
		return;
	in_host_mode = false;

	/* Place interfase in gadget mode */

	/* Disable VBUS power. This means that if a USB host
	   is plugged into the Tegra USB port, then we will 
	   detect the power it supplies and go into gadget 
	   mode */
	gpio_direction_output(SHUTTLE_USB0_VBUS, 0); 

	/* Leave some time for stabilization purposes - This 
	   should unregister all attached devices, as they
	   all lost power */
	msleep(500);

	/* Switch to peripheral mode */
	tegra_otg_set_host_mode(false);
	
}

struct kobject *usb_kobj = NULL;
static ssize_t usb_read(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	int ret = 0;
	
	if (!strcmp(attr->attr.name, "host_mode")) {
		if (in_host_mode)
			ret = 1;
	}

	return strlcpy(buf, ret ? "1\n" : "0\n", 3);
}

static ssize_t usb_write(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	unsigned long on = simple_strtoul(buf, NULL, 10);

	if (!strcmp(attr->attr.name, "host_mode")) {
		if (on)
			tegra_set_host_mode();
		else
			tegra_set_gadget_mode();
	} 

	return count;
}

static DEVICE_ATTR(host_mode, 0666, usb_read, usb_write); /* Allow everybody to switch mode */

static struct attribute *usb_sysfs_entries[] = {
	&dev_attr_host_mode.attr,
	NULL
};

static struct attribute_group usb_attr_group = {
	.name	= NULL,
	.attrs	= usb_sysfs_entries,
}; 
#endif


int __init shuttle_usb_register_devices(void)
{
	int ret = 0;

#ifdef CONFIG_USB_SUPPORT

	/* OTG should be the first to be registered */
	tegra_otg_device.dev.platform_data = &tegra_otg_pdata;
	tegra_udc_device.dev.platform_data = &tegra_udc_pdata;
	tegra_ehci3_device.dev.platform_data = &tegra_ehci3_utmi_pdata;

	/* If in host mode, set VBUS to 1 */
	gpio_request(SHUTTLE_USB0_VBUS, "USB0 VBUS"); /* VBUS switch, perhaps ? -- Tied to what? -- should require +5v ... */
	
	/* 0 = Gadget */
	gpio_direction_output(SHUTTLE_USB0_VBUS, 0 ); /* Gadget */
	
	ret = platform_add_devices(shuttle_usb_devices, ARRAY_SIZE(shuttle_usb_devices));
	if (ret)
		return ret;

	/* Enable gadget mode by default */
	tegra_set_gadget_mode();
		
	/* Register a sysfs interface to let user switch modes */
	usb_kobj = kobject_create_and_add("usbbus", NULL);
	if (!usb_kobj) {
		pr_err("Unable to register USB mode switch");
		return 0;	
	}
	
	
	/* Attach an attribute to the already registered usbbus to let the user switch usb modes */
	return sysfs_create_group(usb_kobj, &usb_attr_group); 
#else
	return 0;
#endif	
}
