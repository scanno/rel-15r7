/*
 * arch/arm/mach-tegra/board-shuttle-sdhci.c
 *
 * Copyright (C) 2011 Eduardo José Tagle <ejtagle@tutopia.com> 
 * Copyright (C) 2010 Google, Inc.
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

#include <linux/resource.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/version.h>
#include <linux/err.h>

#include <asm/mach-types.h>
#include <mach/irqs.h>
#include <mach/iomap.h>
#include <mach/sdhci.h>
#include <mach/pinmux.h>

#include "gpio-names.h"
#include "devices.h"
#include "board-shuttle.h"

/*
  For Shuttle, 
    SDIO0: WLan
	SDIO1: Missing SD MMC
	sDIO2: Unused
	SDIO3: SD MMC
 */

static void (*wifi_status_cb)(int card_present, void *dev_id) = NULL;
static void *wifi_status_cb_devid = NULL;

/* Called by the SDHCI bus driver to register a callback to signal card status has changed */
static int shuttle_wlan_status_register(
		void (*callback)(int card_present_ignored, void *dev_id),
		void *dev_id)
{
	if (wifi_status_cb)
		return -EAGAIN;
	wifi_status_cb = callback;
	wifi_status_cb_devid = dev_id;
	return 0;
} 

/* Used to set the virtual CD of wifi adapter */
int shuttle_wifi_set_cd(int val)
{
	pr_debug("%s: %d\n", __func__, val);
	
	/* Let the SDIO infrastructure know about the change */
	if (wifi_status_cb) {
		wifi_status_cb(val, wifi_status_cb_devid);
	} else
		pr_info("%s: Nobody to notify\n", __func__);

	return 0;
}
EXPORT_SYMBOL_GPL(shuttle_wifi_set_cd);


struct tegra_sdhci_platform_data shuttle_wlan_data = {
	.mmc_data = {
		.register_status_notify	= shuttle_wlan_status_register, 
		.built_in = 1,
		.ocr_mask = MMC_OCR_1V8_MASK,
	},
	.pm_flags = MMC_PM_KEEP_POWER, /* To support WoW */
	.cd_gpio = -1,
	.wp_gpio = -1,
	.power_gpio = -1,
};

static struct tegra_sdhci_platform_data tegra_sdhci_platform_data2 = {
	.cd_gpio = -1,
	.wp_gpio = -1,
	.power_gpio = -1,
};

static struct tegra_sdhci_platform_data tegra_sdhci_platform_data3 = {
	.cd_gpio = SHUTTLE_SDIO2_CD,
	.wp_gpio = -1,
	.power_gpio = SHUTTLE_SDIO2_POWER,
};


static struct tegra_sdhci_platform_data tegra_sdhci_platform_data4 = {
	.cd_gpio = SHUTTLE_SDHC_CD,
	.cd_gpio_active_high = 1,
	.wp_gpio = SHUTTLE_SDHC_WP,
	.power_gpio = SHUTTLE_SDHC_POWER,

};

static struct platform_device *shuttle_sdhci_devices[] __initdata = {
	&tegra_sdhci_device1,
	&tegra_sdhci_device2,
	&tegra_sdhci_device3,
	&tegra_sdhci_device4,
};

/* Register sdhci devices */
int __init shuttle_sdhci_register_devices(void)
{
	/* Plug in platform data */
	tegra_sdhci_device1.dev.platform_data = &shuttle_wlan_data;
	tegra_sdhci_device2.dev.platform_data = &tegra_sdhci_platform_data2;
	tegra_sdhci_device3.dev.platform_data = &tegra_sdhci_platform_data3;
	tegra_sdhci_device4.dev.platform_data = &tegra_sdhci_platform_data4;

	return platform_add_devices(shuttle_sdhci_devices, ARRAY_SIZE(shuttle_sdhci_devices));
}
