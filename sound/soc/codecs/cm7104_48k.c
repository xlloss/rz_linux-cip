// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2010-2011,2013-2015 The Linux Foundation. All rights reserved.
 *
 * cm7104_48k.c -- cm7104 ALSA SoC Codec driver
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/soc-dapm.h>

#define USE_GPIO_SDMODE 0

struct cm7104_priv {
	struct gpio_desc *hp_dev;
	#if USE_GPIO_SDMODE
	struct gpio_desc *sdmode;
	#endif
	int sdmode_switch;
};

static int cm7104_daiops_trigger(struct snd_pcm_substream *substream,
		int cmd, struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	#if USE_GPIO_SDMODE
	struct cm7104_priv *cm7104 =
		snd_soc_component_get_drvdata(component);

	if (!cm7104->sdmode)
		return 0;
	#endif
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		dev_info(component->dev, "START [cmd %d]\n", cmd);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		dev_info(component->dev, "STOP [cmd %d]\n", cmd);
		break;
	}

	return 0;
}

static int cm7104_sdmode_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component =
		snd_soc_dapm_to_component(w->dapm);
	struct cm7104_priv *cm7104 =
		snd_soc_component_get_drvdata(component);

	if (event & SND_SOC_DAPM_POST_PMU)
		cm7104->sdmode_switch = 1;
	else if (event & SND_SOC_DAPM_POST_PMD)
		cm7104->sdmode_switch = 0;

	return 0;
}

static int amic_aif_event(struct snd_soc_dapm_widget *w,
			  struct snd_kcontrol *kcontrol, int event) {
	/* struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm); */
	/* struct cm7104_priv *cm7104 = snd_soc_component_get_drvdata(component); */

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		break;
	case SND_SOC_DAPM_POST_PMD:
		break;
	}

	return 0;
}

static const struct snd_soc_dapm_widget cm7104_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("Speaker"),
	SND_SOC_DAPM_OUT_DRV_E("SD_MODE", SND_SOC_NOPM, 0, 0, NULL, 0,
			cm7104_sdmode_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_INPUT("AMic"),
	SND_SOC_DAPM_AIF_OUT_E("AIF", "Capture", 0,
				SND_SOC_NOPM, 0, 0, amic_aif_event,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
};

static const struct snd_soc_dapm_route cm7104_dapm_routes[] = {
	{"SD_MODE", NULL, "Playback"},
	{"Speaker", NULL, "SD_MODE"},
	{"AIF", NULL, "Capture"},
};

static const struct snd_soc_component_driver cm7104_component_driver = {
	.dapm_widgets		= cm7104_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(cm7104_dapm_widgets),
	.dapm_routes		= cm7104_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(cm7104_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
	.non_legacy_dai_naming	= 1,
};

static const struct snd_soc_dai_ops cm7104_dai_ops = {
	.trigger        = cm7104_daiops_trigger,
};

static struct snd_soc_dai_driver cm7104_dai_driver = {
	.name = "CM7104-HiFi",
	.playback = {
		.stream_name	= "Playback",
		.formats	= SNDRV_PCM_FMTBIT_S16 |
					SNDRV_PCM_FMTBIT_S24 |
					SNDRV_PCM_FMTBIT_S32,
		.rates		= SNDRV_PCM_RATE_48000,
		.rate_min	= 48000,
		.rate_max	= 48000,
		.channels_min	= 1,
		.channels_max	= 2,
	},

	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
		.formats = SNDRV_PCM_FMTBIT_S32_LE
			| SNDRV_PCM_FMTBIT_S24_LE
			| SNDRV_PCM_FMTBIT_S16_LE,

		.rates		= SNDRV_PCM_RATE_16000,
		.rate_min	= 16000,
		.rate_max	= 16000,
	},

	.ops    = &cm7104_dai_ops,


};

static int cm7104_platform_probe(struct platform_device *pdev)
{
	struct cm7104_priv *cm7104;
	pr_info("%s %d\n", __func__, __LINE__);
	cm7104 = devm_kzalloc(&pdev->dev, sizeof(*cm7104), GFP_KERNEL);
	if (!cm7104)
		return -ENOMEM;
	#if USE_GPIO_SDMODE
	cm7104->sdmode = devm_gpiod_get_optional(&pdev->dev,
				"sdmode", GPIOD_OUT_LOW);
	if (IS_ERR(cm7104->sdmode))
		return PTR_ERR(cm7104->sdmode);

	ret = device_property_read_u32(&pdev->dev, "sdmode-delay",
					&cm7104->sdmode_delay);
	if (ret) {
		cm7104->sdmode_delay = 0;
		dev_dbg(&pdev->dev,
			"no optional property 'sdmode-delay' found, "
			"default: no delay\n");
	}
	#endif

	dev_set_drvdata(&pdev->dev, cm7104);

	return devm_snd_soc_register_component(&pdev->dev,
			&cm7104_component_driver,
			&cm7104_dai_driver, 1);
}

#ifdef CONFIG_OF
static const struct of_device_id cm7104_device_id[] = {
	{ .compatible = "cmedia,cm7104" },
	{}
};
MODULE_DEVICE_TABLE(of, cm7104_device_id);
#endif

#ifdef CONFIG_ACPI
static const struct acpi_device_id cm7104_acpi_match[] = {
	{ "CM7104", 0 },
	{},
};
MODULE_DEVICE_TABLE(acpi, cm7104_acpi_match);
#endif

static struct platform_driver cm7104_platform_driver = {
	.driver = {
		.name = "cm7104",
		.of_match_table = of_match_ptr(cm7104_device_id),
		.acpi_match_table = ACPI_PTR(cm7104_acpi_match),
	},
	.probe	= cm7104_platform_probe,
};
module_platform_driver(cm7104_platform_driver);

MODULE_DESCRIPTION("CMedia CM7104 Codec Driver");
MODULE_LICENSE("GPL v2");
