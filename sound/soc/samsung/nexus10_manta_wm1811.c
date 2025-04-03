// SPDX-License-Identifier: GPL-2.0+
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/mfd/wm8994/registers.h>
#include <linux/mfd/wm8994/pdata.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regulator/consumer.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "i2s.h"
#include "../codecs/wm8994.h"

struct manta_priv {
	struct clk *mclk1;
	struct clk *mclk2;
	unsigned int fll1_rate;
	struct snd_soc_jack headset_jack;
};

static int manta_start_fll1(struct snd_soc_pcm_runtime *rtd, unsigned int rate)
{
	struct snd_soc_card *card = rtd->card;
	struct manta_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);
	unsigned long mclk1_rate;
	int ret;

	if (!rate)
		rate = priv->fll1_rate;
	/*
	 * If no new rate is requested, set FLL1 to a sane default for jack
	 * detection.
	 */
	mclk1_rate = clk_get_rate(priv->mclk1);
	if (!rate)
		rate = mclk1_rate / 2;

	if (rate != priv->fll1_rate && priv->fll1_rate) {
		/*
		 * FLL1's frequency needs to be changed. Make sure that we
		 * have a system clock not derived from the FLL, since we
		 * cannot change the FLL when the system clock is derived
		 * from it.
		 * Set FFL clock to maximum during transition in case AIF2
		 * is active to ensure SYSCLK > 256 x fs
		 */
		ret = snd_soc_dai_set_sysclk(codec, WM8994_SYSCLK_MCLK1,
					     mclk1_rate / 2, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			dev_err(card->dev,
				"Unable to switch away from FLL1: %d\n", ret);
			return ret;
		}
	}

	/* Switch the FLL */
	ret = snd_soc_dai_set_pll(codec, WM8994_FLL1, WM8994_FLL_SRC_MCLK1,
				  mclk1_rate, rate);
	if (ret < 0) {
		dev_err(card->dev, "Failed to set FLL1 rate: %d\n", ret);
		return ret;
	}
	priv->fll1_rate = rate;

	ret = snd_soc_dai_set_sysclk(codec, WM8994_SYSCLK_FLL1, priv->fll1_rate,
				     SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(card->dev, "Failed to set SYSCLK source: %d\n", ret);
		return ret;
	}

	return 0;
}

static int manta_stop_fll1(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct manta_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);
	unsigned long mclk2_rate;
	int ret;

	/* Switch to the slower MCLK2 for reduced power consumption */
	mclk2_rate = clk_get_rate(priv->mclk2);
	ret = snd_soc_dai_set_sysclk(codec, WM8994_SYSCLK_MCLK2, mclk2_rate,
				     SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(codec->dev, "Unable to switch to MCLK2: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_pll(codec, WM8994_FLL1, 0, 0, 0);
	if (ret < 0) {
		dev_err(codec->dev, "Unable to stop FLL1: %d\n", ret);
		return ret;
	}

	priv->fll1_rate = 0;

	return 0;
}

static unsigned int manta_rates[] =
        {8000, 11025, 12000, 16000, 22050, 24000};
static struct snd_pcm_hw_constraint_list manta_constraints_rates = {
        .count = ARRAY_SIZE(manta_rates),
        .list = manta_rates,
};

static int manta_wm1811_startup(struct snd_pcm_substream *substream)
{
	return snd_pcm_hw_constraint_list(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_RATE,
					  &manta_constraints_rates);
}

static int manta_wm1811_aif1_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int prate, rate;

	prate = params_rate(params);

	/*
	 * Ensure clock compatibility with high performance mode of DAC operation.
	 * Note that sample rates > 48kHz are not supported.
	 */
	if (prate <= 24000)
		rate = prate * 512;
	else if (prate <= 48000)
		rate = prate * 256;
	else
		return -EINVAL;

	return manta_start_fll1(rtd, rate);
}

static int manta_wm1811_aif1_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);

	return manta_stop_fll1(rtd);
}

static struct snd_soc_ops manta_wm1811_aif1_ops = {
	.startup = manta_wm1811_startup,
	.hw_params = manta_wm1811_aif1_hw_params,
	.hw_free = manta_wm1811_aif1_hw_free,
};

SND_SOC_DAILINK_DEFS(wm1811_pri,
	DAILINK_COMP_ARRAY(COMP_CPU(SAMSUNG_I2S_DAI)),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "wm8994-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link manta_dai[] = {
	{
		.name = "WM1811 PRIMARY",
		.stream_name = "Media primary",
		.ops = &manta_wm1811_aif1_ops,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBP_CFP,
		SND_SOC_DAILINK_REG(wm1811_pri),
	},
};

static int manta_set_bias_level(struct snd_soc_card *card,
				struct snd_soc_dapm_context *dapm,
				enum snd_soc_bias_level level)
{
	struct snd_soc_pcm_runtime *rtd =
		snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);

	if (dapm->dev != codec->dev)
		return 0;

	if ((level == SND_SOC_BIAS_PREPARE) &&
	    (dapm->bias_level == SND_SOC_BIAS_STANDBY))
		return manta_start_fll1(rtd, 0);

	return 0;
}

static int manta_set_bias_level_post(struct snd_soc_card *card,
				     struct snd_soc_dapm_context *dapm,
				     enum snd_soc_bias_level level)
{
	struct snd_soc_pcm_runtime *rtd =
		snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);
	int ret = 0;

	if (dapm->dev != codec->dev)
		return 0;

	if (level == SND_SOC_BIAS_STANDBY)
		ret = manta_stop_fll1(rtd);

	dapm->bias_level = level;

	return ret;
}

static int manta_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd =
		snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);
	struct manta_priv *priv = snd_soc_card_get_drvdata(card);
	unsigned long mclk2_rate;
	int ret;

	/*
	 * Hack: permit the codec to open streams with the same number
	 * of channels that the CPU DAI (samsung-i2s) supports, since
	 * the HDMI block takes its audio from the i2s0 channel shared
	 * with the codec.
	 */
	codec->driver->playback.channels_max =
		cpu_dai->driver->playback.channels_max;

	mclk2_rate = clk_get_rate(priv->mclk2);
	ret = snd_soc_dai_set_sysclk(codec, WM8994_SYSCLK_MCLK2, mclk2_rate,
				     SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(codec->dev, "Unable to switch to MCLK2 (%d)\n", ret);
		return ret;
	}

	/* Force AIF1CLK on as it will be master for jack detection */
	ret = snd_soc_dapm_force_enable_pin(&card->dapm, "AIF1CLK");
	if (ret < 0) {
		dev_err(codec->dev, "Failed to enable AIF1CLK (%d)\n", ret);
		return ret;
	}

	ret = snd_soc_card_jack_new(card, "Headset",
				    SND_JACK_HEADSET |
				    SND_JACK_MECHANICAL |
				    SND_JACK_BTN_0 |
				    SND_JACK_BTN_1 |
				    SND_JACK_BTN_2,
				    &priv->headset_jack);
	if (ret)
		dev_warn(codec->dev, "Failed to create jack (%d)\n", ret);

	/*
	 * Settings provided by Wolfson for Samsung-specific customization
	 * of MICBIAS levels
	 */
	snd_soc_component_write(codec->component, 0x102, 0x3);
	snd_soc_component_write(codec->component, 0xcb, 0x5151);
	snd_soc_component_write(codec->component, 0xd3, 0x3f3f);
	snd_soc_component_write(codec->component, 0xd4, 0x3f3f);
	snd_soc_component_write(codec->component, 0xd5, 0x3f3f);
	snd_soc_component_write(codec->component, 0xd6, 0x3226);
	snd_soc_component_write(codec->component, 0x102, 0x0);
	snd_soc_component_write(codec->component, 0xd1, 0x87);
	snd_soc_component_write(codec->component, 0x3b, 0x9);
	snd_soc_component_write(codec->component, 0x3c, 0x2);

	ret = snd_jack_set_key(priv->headset_jack.jack, SND_JACK_BTN_0,
			       KEY_MEDIA);
	if (ret < 0)
		dev_warn(codec->dev, "Failed to set KEY_MEDIA (%d)\n", ret);

	ret = snd_jack_set_key(priv->headset_jack.jack, SND_JACK_BTN_1,
			       KEY_VOLUMEUP);
	if (ret < 0)
		dev_warn(codec->dev, "Failed to set KEY_VOLUMEUP: (%d)\n", ret);

	ret = snd_jack_set_key(priv->headset_jack.jack, SND_JACK_BTN_2,
			       KEY_VOLUMEDOWN);
	if (ret < 0)
		dev_warn(codec->dev, "Failed to set KEY_VOLUMEDOWN: (%d)\n",
			 ret);

	wm8958_mic_detect(codec->component, &priv->headset_jack, NULL, NULL,
			  NULL, NULL);

	return 0;
}

static int manta_card_suspend_post(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd =
		snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);
	struct manta_priv *priv = snd_soc_card_get_drvdata(card);

	snd_soc_component_update_bits(codec->component,
				      WM8994_AIF1_MASTER_SLAVE,
				      WM8994_AIF1_TRI_MASK,
				      WM8994_AIF1_TRI);

	clk_disable_unprepare(priv->mclk1);
	clk_disable_unprepare(priv->mclk2);

	return 0;
}

static int manta_card_resume_pre(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd =
		snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);
	struct manta_priv *priv = snd_soc_card_get_drvdata(card);
	int err;

	err = clk_prepare_enable(priv->mclk1);
	if (err < 0) {
		dev_err(codec->dev, "Failed to enable clock MCLK1 (%d)\n", err);
		return err;
	}
	err = clk_prepare_enable(priv->mclk2);
	if (err < 0) {
		dev_err(codec->dev, "Failed to enable clock MCLK2 (%d)\n", err);
		clk_disable_unprepare(priv->mclk1);
		return err;
	}

	snd_soc_component_update_bits(codec->component,
				      WM8994_AIF1_MASTER_SLAVE,
				      WM8994_AIF1_TRI_MASK, 0);

	return 0;
}

static struct snd_soc_card manta_card = {
	.owner = THIS_MODULE,
	.dai_link = manta_dai,
	.num_links = ARRAY_SIZE(manta_dai),

	.set_bias_level = manta_set_bias_level,
	.set_bias_level_post = manta_set_bias_level_post,

	.late_probe = manta_late_probe,

	.suspend_post = manta_card_suspend_post,
	.resume_pre = manta_card_resume_pre,
};

static int manta_probe(struct platform_device *pdev)
{
	struct device_node *cpu = NULL, *codec = NULL;
	struct snd_soc_card *card = &manta_card;
	struct device *dev = &pdev->dev;
	struct manta_priv *priv;
	int ret = 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	snd_soc_card_set_drvdata(card, priv);
	card->dev = dev;

	ret = snd_soc_of_parse_card_name(card, "model");
	if (ret < 0) {
		dev_err(dev, "Failed to parse model property (%d)\n", ret);
		goto err;
	}

	ret = snd_soc_of_parse_pin_switches(card, "samsung,pin-switches");
	if (ret < 0) {
		dev_err(dev, "Failed to parse pin-switches property (%d)\n", ret);
		goto err;
	}

	ret = snd_soc_of_parse_audio_simple_widgets(card, "samsung,audio-widgets");
	if (ret < 0) {
		dev_err(dev, "Failed to parse audio-widgets property (%d)\n", ret);
		goto err;
	}

	ret = snd_soc_of_parse_audio_routing(card, "audio-routing");
	if (ret < 0) {
		dev_err(dev, "Failed to parse audio-routing property (%d)\n", ret);
		goto err;
	}

	ret = snd_soc_of_parse_aux_devs(card, "samsung,aux-devs");
	if (ret < 0) {
		dev_err(dev, "Failed to parse aux-devs property (%d)\n", ret);
		goto err;
	}

	cpu = of_get_child_by_name(dev->of_node, "cpu");
	if (!cpu) {
		dev_err(dev, "audio-cpu property invalid/missing\n");
		ret = -EINVAL;
		goto err;
	}

	manta_dai[0].cpus->of_node = of_parse_phandle(cpu, "sound-dai", 0);
	if (!manta_dai[0].cpus->of_node) {
		dev_err(dev, "Failed parsing cpu node\n");
		ret = -EINVAL;
		goto err_put_cpu;
	}

	codec = of_get_child_by_name(dev->of_node, "codec");
	if (!codec) {
		dev_err(dev, "audio-codec property invalid/missing\n");
		ret = -EINVAL;
		goto err_put_dai_link_cpus;
	}

	manta_dai[0].codecs->of_node = of_parse_phandle(codec, "sound-dai", 0);
	if (!manta_dai[0].codecs->of_node) {
		dev_err(dev, "Failed parsing codec node\n");
		ret = -EINVAL;
		goto err_put_codec;
	}

	priv->mclk1 = of_clk_get_by_name(manta_dai[0].codecs->of_node, "MCLK1");
	if (IS_ERR(priv->mclk1)) {
		dev_err(dev, "Failed to get clock MCLK1\n");
		ret = PTR_ERR(priv->mclk1);
		goto err_put_dai_link_codecs;
	}

	ret = clk_prepare_enable(priv->mclk1);
	if (ret < 0) {
		dev_err(dev, "Failed to enable clock MCLK1 (%d)\n", ret);
		goto err_put_mclk1;
	}

	priv->mclk2 = of_clk_get_by_name(manta_dai[0].codecs->of_node, "MCLK2");
	if (IS_ERR(priv->mclk2)) {
		dev_err(dev, "Failed to get clock MCLK2\n");
		ret = PTR_ERR(priv->mclk2);
		goto err_disable_mclk1;
	}

	ret = clk_prepare_enable(priv->mclk2);
	if (ret < 0) {
		dev_err(dev, "Failed to enable clock MCLK2 (%d)\n", ret);
		goto err_put_mclk2;
	}

	snd_soc_dlc_use_cpu_as_platform(manta_dai[0].platforms, manta_dai[0].cpus);

	ret = devm_snd_soc_register_card(dev, card);
	if (ret < 0) {
		dev_err(dev, "Failed to register card (%d)\n", ret);
		goto err_disable_mclk2;
	}

	of_node_put(codec);
	of_node_put(cpu);

	return 0;

err_disable_mclk2:
	clk_disable_unprepare(priv->mclk2);
err_put_mclk2:
	clk_put(priv->mclk2);
err_disable_mclk1:
	clk_disable_unprepare(priv->mclk1);
err_put_mclk1:
	clk_put(priv->mclk1);
err_put_dai_link_codecs:
	of_node_put(manta_dai[0].codecs->of_node);
err_put_codec:
	of_node_put(codec);
err_put_dai_link_cpus:
	of_node_put(manta_dai[0].cpus->of_node);
err_put_cpu:
	of_node_put(cpu);
err:
	return ret;
}

static void manta_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct manta_priv *priv = snd_soc_card_get_drvdata(card);

	of_node_put(manta_dai[0].cpus->of_node);
	of_node_put(manta_dai[0].codecs->of_node);

	clk_disable_unprepare(priv->mclk2);
	clk_put(priv->mclk2);

	clk_disable_unprepare(priv->mclk1);
	clk_put(priv->mclk1);
}

static const struct of_device_id manta_of_match[] = {
	{ .compatible = "samsung,nexus10-manta-audio" },
	{},
};
MODULE_DEVICE_TABLE(of, manta_of_match);

static struct platform_driver manta_driver = {
	.driver = {
		.name = "nexus10-manta-audio",
		.of_match_table = manta_of_match,
		.pm = &snd_soc_pm_ops,
	},
	.probe = manta_probe,
	.remove = manta_remove,
};
module_platform_driver(manta_driver);

MODULE_DESCRIPTION("ASoC support for Samsung Nexus 10 (Manta)");
MODULE_LICENSE("GPL");
