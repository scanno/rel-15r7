/*
 * tegra_alc5624.c  --  SoC audio for tegra (glue logic)
 *
 * (c) 2010-2011 Nvidia Graphics Pvt. Ltd.
 *  http://www.nvidia.com
 * (C) 2011-2012 Eduardo José Tagle <ejtagle@tutopia.com>
 *
 * Copyright 2007 Wolfson Microelectronics PLC.
 * Author: Graeme Gregory
 *         graeme.gregory@wolfsonmicro.com or linux@wolfsonmicro.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

/* Shuttle uses MIC2 as the mic input */
 
#define DEBUG
#include <asm/mach-types.h>

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif
#include <linux/clk.h>
#include <linux/tegra_audio.h>
#include <linux/delay.h>

#include <mach/clk.h>
#include <mach/shuttle_audio.h>

#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/jack.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include <sound/alc5624.h>

#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
#include "tegra20_das.h"
#endif

#define DRV_NAME "tegra-snd-alc5624"

struct tegra_alc5624 {
	struct tegra_asoc_utils_data util_data;
	enum snd_soc_bias_level 		bias_level;
	int					  gpio_hp_det;		/* GPIO used to detect Headphone plugged in */
	int					  hifi_codec_datafmt;/* HiFi codec data format */
	bool				  hifi_codec_master;/* If Hifi codec is master */
	int					  bt_codec_datafmt;	/* Bluetooth codec data format */
	bool				  bt_codec_master;	/* If bt codec is master */
};

/* mclk required for each sampling frequency */
static const struct {
	unsigned int mclk;
	unsigned int srate;
} clocktab[] = {
	/* 8k */
	{ 8192000,  8000},
	{12288000,  8000},
	{24576000,  8000},

	/* 11.025k */
	{11289600, 11025},
	{16934400, 11025},
	{22579200, 11025},

	/* 16k */
	{12288000, 16000},
	{16384000, 16000},
	{24576000, 16000},

	/* 22.05k */	
	{11289600, 22050},
	{16934400, 22050},
	{22579200, 22050},

	/* 32k */
	{12288000, 32000},
	{16384000, 32000},
	{24576000, 32000},

	/* 44.1k */
	{11289600, 44100},
	{22579200, 44100},

	/* 48k */
	{12288000, 48000},
	{24576000, 48000},
};


/* --------- Digital audio interfase ------------ */

static int tegra_alc5624_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai 	= rtd->codec_dai;
	struct snd_soc_dai *cpu_dai 	= rtd->cpu_dai;
	struct snd_soc_codec *codec		= rtd->codec;
	struct snd_soc_card* card		= codec->card;
	struct tegra_alc5624* machine 	= snd_soc_card_get_drvdata(card);
	
	int sys_clk = 12000000;
	int err;
	int i;	
	bool codec_is_master;
	int dai_flag;

	/* Get the requested sampling rate */
	unsigned int srate = params_rate(params);
	
	/* Look for the required mclk for the requested sampling rate */
	for (i = 0; i < ARRAY_SIZE(clocktab); i++) {
		if (clocktab[i].srate == srate) {
			sys_clk = clocktab[i].mclk;
			break;
		}
	}
	
	/* Set the required clock rate */
	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, sys_clk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % sys_clk))
			sys_clk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev,"%s(): unable to get required %d hz sampling rate\n",__FUNCTION__,srate);		
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);	

	/* I2S <-> DAC <-> DAS <-> DAP <-> CODEC
	   -If DAP is master, codec will be slave */
	codec_is_master = machine->hifi_codec_master;
	
	/* Get DAS dataformat - DAP is connecting to codec */
	dai_flag = machine->hifi_codec_datafmt;
	
	/* Depending on the number of channels, we must select the mode -
		I2S only supports stereo operation, DSP_A can support mono 
		with the ALC5624 */
	/*t dai_flag = (params_channels(params) == 1) 
						? SND_SOC_DAIFMT_DSP_A
						: SND_SOC_DAIFMT_I2S;*/
	
	dev_dbg(card->dev,"%s(): cpu_dai:'%s'\n", __FUNCTION__,cpu_dai->name);
	dev_dbg(card->dev,"%s(): codec_dai:'%s'\n", __FUNCTION__,codec_dai->name);
	
	if (codec_is_master)
		dai_flag |= SND_SOC_DAIFMT_CBM_CFM; /* codec is master */
	else
		dai_flag |= SND_SOC_DAIFMT_CBS_CFS;

	dev_dbg(card->dev,"%s(): format: 0x%08x, channels:%d, srate:%d\n", __FUNCTION__,
		params_format(params),params_channels(params),params_rate(params));

	/* Set the CPU dai format. This will also set the clock rate in master mode */
	err = snd_soc_dai_set_fmt(cpu_dai, dai_flag);
	if (err < 0) {
		dev_err(card->dev,"cpu_dai fmt not set \n");
		return err;
	}

	err = snd_soc_dai_set_fmt(codec_dai, dai_flag);
	if (err < 0) {
		dev_err(card->dev,"codec_dai fmt not set \n");
		return err;
	}

	if (codec_is_master) {
		dev_dbg(card->dev,"%s(): codec in master mode\n",__FUNCTION__);
		
		/* If using port as slave (=codec as master), then we can use the
		   codec PLL to get the other sampling rates */
		
		/* Try each one until success */
		for (i = 0; i < ARRAY_SIZE(clocktab); i++) {
		
			if (clocktab[i].srate != srate) 
				continue;
				
			if (snd_soc_dai_set_pll(codec_dai, 0, 0, sys_clk, clocktab[i].mclk) >= 0) {
				/* Codec PLL is synthetizing this new clock */
				sys_clk = clocktab[i].mclk;
				break;
			}
		}
		
		if (i >= ARRAY_SIZE(clocktab)) {
			dev_err(card->dev,"%s(): unable to set required MCLK for SYSCLK of %d, sampling rate: %d\n",__FUNCTION__,sys_clk,srate);
			return -EINVAL;
		}
		
	} else {
		dev_dbg(card->dev,"%s(): codec in slave mode\n",__FUNCTION__);

		/* Disable codec PLL */
		err = snd_soc_dai_set_pll(codec_dai, 0, 0, 0, 0);
		if (err < 0) {
			dev_err(card->dev,"%s(): unable to disable codec PLL\n",__FUNCTION__);
			return err;
		}
	}
	
	/* Set CODEC sysclk */
	err = snd_soc_dai_set_sysclk(codec_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev,"codec_dai clock not set\n");
		return err;
	}

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	err = tegra20_das_connect_dac_to_dap(TEGRA20_DAS_DAP_SEL_DAC1,
					TEGRA20_DAS_DAP_ID_1);
	if (err < 0) {
		dev_err(card->dev, "failed to set dap-dac path\n");
		return err;
	}

	err = tegra20_das_connect_dap_to_dac(TEGRA20_DAS_DAP_ID_1,
					TEGRA20_DAS_DAP_SEL_DAC1);
	if (err < 0) {
		dev_err(card->dev, "failed to set dac-dap path\n");
		return err;
	}
#endif
	
	return 0;
}

static int tegra_bt_sco_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai 	= rtd->codec_dai;
	struct snd_soc_dai *cpu_dai 	= rtd->cpu_dai;
	struct snd_soc_codec *codec		= rtd->codec;
	struct snd_soc_card* card		= codec->card;
	struct tegra_alc5624* machine 	= snd_soc_card_get_drvdata(card);
	
	int sys_clk, srate, min_mclk;
	int err;

	/* Get DAS dataformat and master flag */
	int codec_is_master = machine->bt_codec_master;

	/* We are supporting DSP and I2s format for now */
	int dai_flag = machine->bt_codec_datafmt;

	if (codec_is_master)
		dai_flag |= SND_SOC_DAIFMT_CBM_CFM; /* codec is master */
	else
		dai_flag |= SND_SOC_DAIFMT_CBS_CFS;

	dev_dbg(card->dev,"%s(): format: 0x%08x, channels:%d, srate:%d\n", __FUNCTION__,
		params_format(params),params_channels(params),params_rate(params));

	/* Set the CPU dai format. This will also set the clock rate in master mode */
	err = snd_soc_dai_set_fmt(cpu_dai, dai_flag);
	if (err < 0) {
		dev_err(card->dev,"cpu_dai fmt not set \n");
		return err;
	}

	/* Bluetooth Codec is always slave here */
	err = snd_soc_dai_set_fmt(codec_dai, dai_flag);
	if (err < 0) {
		dev_err(card->dev,"codec_dai fmt not set \n");
		return err;
	}
	
	/* Get system clock */
	srate = params_rate(params);
	switch (srate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		sys_clk = 11289600;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
		sys_clk = 12288000;
		break;
	default:
		return -EINVAL;
	}
	min_mclk = 64 * srate;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, sys_clk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % min_mclk))
			sys_clk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	/* Set CPU sysclock as the same - in Tegra, seems to be a NOP */
	err = snd_soc_dai_set_sysclk(cpu_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev,"cpu_dai clock not set\n");
		return err;
	}
	
	/* Set CODEC sysclk */
	err = snd_soc_dai_set_sysclk(codec_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev,"cpu_dai clock not set\n");
		return err;
	}
	
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	err = tegra20_das_connect_dac_to_dap(TEGRA20_DAS_DAP_SEL_DAC2,
					TEGRA20_DAS_DAP_ID_4);
	if (err < 0) {
		dev_err(card->dev, "failed to set dac-dap path\n");
		return err;
	}

	err = tegra20_das_connect_dap_to_dac(TEGRA20_DAS_DAP_ID_4,
					TEGRA20_DAS_DAP_SEL_DAC2);
	if (err < 0) {
		dev_err(card->dev, "failed to set dac-dap path\n");
		return err;
	}
#endif	
	
	return 0;
}

static int tegra_spdif_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec		= rtd->codec;
	struct snd_soc_card* card		= codec->card;
	struct tegra_alc5624* machine 	= snd_soc_card_get_drvdata(card);
	int srate, mclk, min_mclk;
	int err;

	dev_dbg(card->dev,"%s(): format: 0x%08x\n", __FUNCTION__,params_format(params));

	srate = params_rate(params);
	switch (srate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		mclk = 11289600;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
		mclk = 12288000;
		break;
	default:
		return -EINVAL;
	}
	min_mclk = 128 * srate;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % min_mclk))
			mclk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	return 0;
}

static int tegra_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_alc5624 *machine = snd_soc_card_get_drvdata(rtd->card);

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 0);

	return 0;
}

static struct snd_soc_ops tegra_alc5624_ops = {
	.hw_params = tegra_alc5624_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_ops tegra_bt_sco_ops = {
	.hw_params = tegra_bt_sco_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_ops tegra_spdif_ops = {
	.hw_params = tegra_spdif_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_jack tegra_alc5624_hp_jack;
static struct snd_soc_jack_gpio tegra_alc5624_hp_jack_gpio = {
	.name = "headphone detect",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 150,
	/*.gpio is filled in initialization from platform data */
};

#ifdef CONFIG_SWITCH
/* These values are copied from Android WiredAccessoryObserver */
enum headset_state {
	BIT_NO_HEADSET = 0,
	BIT_HEADSET = (1 << 0),
	BIT_HEADSET_NO_MIC = (1 << 1),
};

static struct switch_dev tegra_alc5624_headset_switch = {
	.name = "h2w",
};

static int tegra_alc5624_jack_notifier(struct notifier_block *self,
			      unsigned long action, void *dev)
{
	struct snd_soc_jack *jack = dev;
	struct snd_soc_codec *codec = jack->codec;
	struct snd_soc_card *card = codec->card;
	int state = 0;

	dev_dbg(card->dev,"Jack notifier: Action: 0x%08x\n",action);
	
	switch (action) {
	case SND_JACK_HEADPHONE:
		state |= BIT_HEADSET_NO_MIC;
		break;
	default:
		state |= BIT_NO_HEADSET;
	}

	switch_set_state(&tegra_alc5624_headset_switch, state);

	return NOTIFY_OK;
}

static struct notifier_block tegra_alc5624_jack_detect_nb = {
	.notifier_call = tegra_alc5624_jack_notifier,
};

#else
/* ------- Headphone jack autodetection  -------- */

static struct snd_soc_jack_pin tegra_jack_pins[] = {
	/* Disable speaker when headphone is plugged in */
	{
		.pin = "Internal Speaker",
		.mask = SND_JACK_HEADPHONE,
		.invert = 1, /* Enable pin when status is not reported */
	},
	/* Enable headphone when status is reported */
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
		.invert = 0, /* Enable pin when status is reported */
	},
};
#endif


/*tegra machine dapm widgets */
static struct snd_soc_dapm_widget tegra_alc5624_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Internal Speaker", NULL),
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Internal Mic", NULL),
};

/* Tegra machine audio map (connections to the codec pins) */
static struct snd_soc_dapm_route shuttle_audio_map[] = {
	{"Headphone Jack", NULL, "HPR"},
	{"Headphone Jack", NULL, "HPL"},
	{"Internal Speaker", NULL, "SPKL"},
	{"Internal Speaker", NULL, "SPKLN"},
	{"Internal Speaker", NULL, "SPKR"},
	{"Internal Speaker", NULL, "SPKRN"},
	{"Mic Bias2", NULL, "Internal Mic"},
	{"MIC2", NULL, "Mic Bias2"},
};

static struct snd_kcontrol_new tegra_alc5624_controls[] = {
	SOC_DAPM_PIN_SWITCH("Internal Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Internal Mic"),
};

static int tegra_alc5624_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec* codec = rtd->codec;
	struct snd_soc_dapm_context* dapm = &codec->dapm;
	struct snd_soc_card *card = codec->card;
	struct tegra_alc5624* machine = snd_soc_card_get_drvdata(card);
	int ret = 0;
	
	machine->bias_level = SND_SOC_BIAS_STANDBY;
	
	/* Store the GPIO used to detect headphone */
	tegra_alc5624_hp_jack_gpio.gpio = machine->gpio_hp_det;

	/* Headphone jack detection */		
	ret = snd_soc_jack_new(codec, "Headphone Jack", SND_JACK_HEADPHONE,
			 &tegra_alc5624_hp_jack);
	if (ret) {
		dev_err(card->dev,"Unable to register jack\n");
		return ret;
	}
	
#ifndef CONFIG_SWITCH
	/* Everytime the Jack status changes, update the DAPM pin status */
	snd_soc_jack_add_pins(&tegra_alc5624_hp_jack,
			  ARRAY_SIZE(tegra_jack_pins),
			  tegra_jack_pins);
#else		 
	/* Everytime the Jack status changes, notify our listerners */
	snd_soc_jack_notifier_register(&tegra_alc5624_hp_jack,
				&tegra_alc5624_jack_detect_nb);
#endif
	/* Everytime the Jack detect gpio changes, report a Jack status change */
	ret = snd_soc_jack_add_gpios(&tegra_alc5624_hp_jack,
			   1,
			   &tegra_alc5624_hp_jack_gpio);
	if (ret)
		return ret;

	/* Set endpoints to not connected */
	snd_soc_dapm_nc_pin(dapm, "LINEL");
	snd_soc_dapm_nc_pin(dapm, "LINER");
	snd_soc_dapm_nc_pin(dapm, "PHONEIN");
	snd_soc_dapm_nc_pin(dapm, "MIC1");
	snd_soc_dapm_nc_pin(dapm, "MONO");

	/* Set endpoints to default on mode */
	snd_soc_dapm_enable_pin(dapm, "Internal Speaker");
	snd_soc_dapm_enable_pin(dapm, "Internal Mic");
	snd_soc_dapm_enable_pin(dapm, "Headphone Jack");

	snd_soc_dapm_force_enable_pin(dapm, "Mic Bias2");		
			  
	ret = snd_soc_dapm_sync(dapm);
	if (ret) {
		dev_err(card->dev,"Failed to sync\n");
		return ret;
	}
	
	return 0;
}

static struct snd_soc_dai_link tegra_alc5624_dai[] = {
	{
		.name = "ALC5624",
		.stream_name = "ALC5624 HiFi",
		.codec_name = "alc5624.0-0018",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-i2s.0",
		.codec_dai_name = "alc5624-hifi",
		.init = tegra_alc5624_init,
		.ops = &tegra_alc5624_ops,
	},
	{
		.name = "BT-SCO",
		.stream_name = "BT SCO PCM",
		.codec_name = "spdif-dit.1",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-i2s.1",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_bt_sco_ops,
	},
	{
		.name = "SPDIF",
		.stream_name = "SPDIF PCM",
		.codec_name = "spdif-dit.0",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-spdif",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_spdif_ops,
	},
};

static int tegra_alc5624_suspend_post(struct snd_soc_card *card)
{
	struct snd_soc_jack_gpio *gpio = &tegra_alc5624_hp_jack_gpio;
	struct tegra_alc5624 *machine = snd_soc_card_get_drvdata(card);

	if (gpio_is_valid(gpio->gpio))
		disable_irq(gpio_to_irq(gpio->gpio));

	/* Disable the codec clock */
	tegra_asoc_utils_clk_disable(&machine->util_data);
	return 0;
}

static int tegra_alc5624_resume_pre(struct snd_soc_card *card)
{
	int val;
	struct snd_soc_jack_gpio *gpio = &tegra_alc5624_hp_jack_gpio;
	struct tegra_alc5624 *machine = snd_soc_card_get_drvdata(card);

   /* Delay on resume because otherwise a paging error 
    * occurs. There is probably a deeper problem here,
    * but this works.
    */
   
	msleep(100);

	/* Enable the codec clock */
	tegra_asoc_utils_clk_enable(&machine->util_data);
	
	if (gpio_is_valid(gpio->gpio)) {
		val = gpio_get_value(gpio->gpio);
		val = gpio->invert ? !val : val;
		snd_soc_jack_report(gpio->jack, val, gpio->report);
		enable_irq(gpio_to_irq(gpio->gpio));
	}

	return 0;
}

static struct snd_soc_card snd_soc_tegra_alc5624 = {
	.name		= "tegra-alc5624",
	.dai_link 	= tegra_alc5624_dai,
	.num_links 	= ARRAY_SIZE(tegra_alc5624_dai),
	.suspend_post = tegra_alc5624_suspend_post,
	.resume_pre = tegra_alc5624_resume_pre,
	.controls = tegra_alc5624_controls,
	.num_controls = ARRAY_SIZE(tegra_alc5624_controls),
	.dapm_widgets = tegra_alc5624_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tegra_alc5624_dapm_widgets),
	.dapm_routes = shuttle_audio_map,
	.num_dapm_routes = ARRAY_SIZE(shuttle_audio_map),
};

/* initialization */
static __devinit int tegra_snd_shuttle_probe(struct platform_device *pdev)
{
	struct snd_soc_card* card = &snd_soc_tegra_alc5624;
	struct tegra_alc5624 *machine;
	struct shuttle_audio_platform_data* pdata;	
	int ret = 0;
	
	dev_dbg(&pdev->dev, "Shuttle sound card probe\n");
	
	/* Get platform data */
	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "no platform data supplied\n");
		return -EINVAL;
	}

	/* Allocate private context */
	machine = kzalloc(sizeof(struct tegra_alc5624), GFP_KERNEL);
	if (!machine) {
		dev_err(&pdev->dev, "Can't allocate tegra_alc5624\n");
		return -ENOMEM;
	}
	
	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev, card);
	if (ret) {
		dev_err(&pdev->dev, "Can't initialize Tegra ASOC utils\n");
		goto err_free_machine;
	}
	
	dev_dbg(&pdev->dev, "Shuttle MCLK initialized\n");
	
#ifdef CONFIG_SWITCH
	/* Add h2w switch class support */
	ret = switch_dev_register(&tegra_alc5624_headset_switch);
	if (ret < 0) {
		dev_err(&pdev->dev, "Can't register SWITCH device\n");
		goto err_fini_utils;
	}
#endif

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card); 
	snd_soc_card_set_drvdata(card, machine);
	
	/* Fill in the GPIO used to detect the headphone */
	machine->gpio_hp_det = pdata->gpio_hp_det;
	machine->hifi_codec_datafmt = pdata->hifi_codec_datafmt;	/* HiFi codec data format */
	machine->hifi_codec_master = pdata->hifi_codec_master;		/* If Hifi codec is master */
	machine->bt_codec_datafmt = pdata->bt_codec_datafmt;		/* Bluetooth codec data format */
	machine->bt_codec_master = pdata->bt_codec_master;			/* If bt codec is master */
	
	/* Add the device */
	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",ret);
		goto err_unregister_switch;
	}

	if (!card->instantiated) {
		ret = -ENODEV;
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err_unregister_card;
	}

	dev_info(&pdev->dev, "Shuttle sound card registered\n");

	return 0;
	
err_unregister_card:
	snd_soc_unregister_card(card);
err_unregister_switch:
#ifdef CONFIG_SWITCH
	switch_dev_unregister(&tegra_alc5624_headset_switch);
#endif
err_fini_utils:
	tegra_asoc_utils_fini(&machine->util_data);
err_free_machine:
	kfree(machine);
	return ret;
	
}

static int __devexit tegra_snd_shuttle_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_alc5624 *machine = snd_soc_card_get_drvdata(card);
	
	snd_soc_unregister_card(card);
	
#ifdef CONFIG_SWITCH
	switch_dev_unregister(&tegra_alc5624_headset_switch);
#endif

	tegra_asoc_utils_fini(&machine->util_data);
	
	kfree(machine);
	
	return 0;
}

static struct platform_driver tegra_snd_shuttle_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
	},
	.probe = tegra_snd_shuttle_probe,
	.remove = __devexit_p(tegra_snd_shuttle_remove),
};

static int __init snd_tegra_shuttle_init(void)
{
	return platform_driver_register(&tegra_snd_shuttle_driver);
}
module_init(snd_tegra_shuttle_init);

static void __exit snd_tegra_shuttle_exit(void)
{
	platform_driver_unregister(&tegra_snd_shuttle_driver);
}
module_exit(snd_tegra_shuttle_exit);

MODULE_AUTHOR("Eduardo José Tagle <ejtagle@tutopia.com>");
MODULE_DESCRIPTION("Shuttle machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
