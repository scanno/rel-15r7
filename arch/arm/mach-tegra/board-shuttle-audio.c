/*
 * arch/arm/mach-tegra/board-shuttle-audio.c
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

/* All configurations related to audio */
 
#include <linux/console.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/i2c-tegra.h>
#include <linux/i2c.h>
#include <linux/version.h>
#include <sound/alc5624.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/time.h>
#include <asm/setup.h>
#include <asm/io.h>

#include <mach/io.h>
#include <mach/irqs.h>
#include <mach/iomap.h>
#include <mach/gpio.h>
#include <mach/i2s.h>
#include <mach/spdif.h>
#include <mach/audio.h>  

#include <mach/system.h>
#include <mach/shuttle_audio.h>

#include "board.h"
#include "board-shuttle.h"
#include "gpio-names.h"
#include "devices.h"

/* Default music path: I2S1(DAC1)<->Dap1<->HifiCodec
   Bluetooth to codec: I2S2(DAC2)<->Dap4<->Bluetooth
*/
/* For Shuttle, 
	Codec is ALC5624
	Codec I2C Address = 0x30(includes R/W bit), i2c #0
	Codec MCLK = APxx DAP_MCLK1
	
	Bluetooth is always master
*/


static struct alc5624_platform_data alc5624_pdata = {
	.avdd_mv			= 3300,	/* Analog vdd in millivolts */
	.spkvdd_mv 			= 5000,	/* Speaker Vdd in millivolts */
	.hpvdd_mv 			= 3300,	/* Headphone Vdd in millivolts */
	.spkvol_scale 		= 88,	/* Scale speaker volume to the percent of maximum range -Be careful: range is logarithmic! */
	
	.mic1bias_mv		= 2970,	/* MIC1 bias voltage */
	.mic2bias_mv		= 2970,	/* MIC2 bias voltage */
	.mic1boost_db		= 30,	/* MIC1 gain boost */
	.mic2boost_db		= 30,	/* MIC2 gain boost */
	
	.default_is_mic2 	= true,	/* Shuttle uses MIC2 as the default capture source */
	
};

static struct i2c_board_info __initdata shuttle_i2c_bus0_board_info[] = {
	{
		I2C_BOARD_INFO("alc5624", 0x18),
		.platform_data = &alc5624_pdata,
	},
};

static struct shuttle_audio_platform_data shuttle_audio_pdata = {
	.gpio_hp_det 		= SHUTTLE_HP_DETECT,
	.hifi_codec_datafmt = SND_SOC_DAIFMT_I2S,	/* HiFi codec data format */
#ifdef ALC5624_IS_MASTER
	.hifi_codec_master  = true,					/* If Hifi codec is master */
#else
	.hifi_codec_master  = false,				/* If Hifi codec is master */
#endif
	.bt_codec_datafmt	= SND_SOC_DAIFMT_DSP_A,	/* Bluetooth codec data format */
	.bt_codec_master    = true,					/* If bt codec is master */
}; 

static struct platform_device shuttle_audio_device = {
	.name = "tegra-snd-alc5624",
	.id   = -1,
	.dev = {
		.platform_data = &shuttle_audio_pdata,
	}, 
};

static struct platform_device *shuttle_i2s_devices[] __initdata = {
	&tegra_i2s_device1,
	&tegra_i2s_device2,
	&tegra_spdif_device,
	&tegra_das_device,
	&spdif_dit_device,	
	&bluetooth_dit_device,
	&tegra_pcm_device,
	&shuttle_audio_device, /* this must come last, as we need the DAS to be initialized to access the codec registers ! */
};

int __init shuttle_audio_register_devices(void)
{
	int ret;

	/* We NEED the shuttle audio device to be registered FIRST, as it enables the MCLK
	   that is required to get proper access to the codec registers */
	ret = platform_add_devices(shuttle_i2s_devices, ARRAY_SIZE(shuttle_i2s_devices));
	if (ret)
		return ret;

	return i2c_register_board_info(0, shuttle_i2c_bus0_board_info, 
		ARRAY_SIZE(shuttle_i2c_bus0_board_info)); 
}
	
