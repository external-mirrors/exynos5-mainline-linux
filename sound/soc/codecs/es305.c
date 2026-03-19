// SPDX-License-Identifier: GPL-2.0
/*
 * Audience ES305 Voice Processor driver
 *
 * Copyright (C) 2012 Google, Inc.
 * Copyright (C) 2012 Samsung Corporation.
 * Copyright (C) 2025 Alexandre Marquet.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <sound/soc.h>

#define FIRMWARE_ES305			"es305_fw.bin"
#define MAX_CMD_SEND_SIZE		32
#define RETRY_COUNT			5

/* ES305 commands and values */
#define ES305_BOOT			0x0001
#define ES305_BOOT_ACK			0x01

#define ES305_SYNC_POLLING		0x80000000

#define ES305_RESET_IMMEDIATE		0x80020000

#define ES305_SET_POWER_STATE_SLEEP	0x80100001

#define ES305_GET_ALGORITHM_PARM	0x80160000
#define ES305_SET_ALGORITHM_PARM_ID	0x80170000
#define ES305_SET_ALGORITHM_PARM	0x80180000
#define ES305_AEC_MODE			0x0003
#define ES305_TX_AGC			0x0004
#define ES305_RX_AGC			0x0028
#define ES305_TX_NS_LEVEL		0x004b
#define ES305_RX_NS_LEVEL		0x004c
#define ES305_ALGORITHM_RESET		0x001c

#define ES305_GET_VOICE_PROCESSING	0x80430000
#define ES305_SET_VOICE_PROCESSING	0x801c0000

#define ES305_GET_AUDIO_ROUTING		0x80270000
#define ES305_SET_AUDIO_ROUTING		0x80260000

#define ES305_SET_PRESET		0x80310000

#define ES305_GET_MIC_SAMPLE_RATE	0x80500000
#define ES305_SET_MIC_SAMPLE_RATE	0x80510000
#define ES305_8KHZ			0x0008
#define ES305_16KHZ			0x000a
#define ES305_48KHZ			0x0030

#define ES305_DIGITAL_PASSTHROUGH	0x80520000

struct es305_data {
	struct device *dev;
	struct i2c_client *client;
	const struct firmware *fw;

	unsigned int passthrough;

	bool asleep;
	bool device_ready;

	struct gpio_desc *gpio_wakeup;
	struct gpio_desc *gpio_reset;

	struct clk *mclk;
};

static int es305_send_cmd(struct es305_data *es305, u32 command, u16 *response)
{
	u8 send[4];
	u8 recv[4];
	int ret = 0;
	int retry = RETRY_COUNT;

	put_unaligned_be32(command, send);

	ret = i2c_master_send(es305->client, send, 4);
	if (ret < 0) {
		dev_err(es305->dev, "i2c_master_send failed (%d)\n", ret);
		return ret;
	}

	/* The sleep command cannot be acked before the device goes to sleep */
	if (command == ES305_SET_POWER_STATE_SLEEP)
		return ret;

	usleep_range(1000, 2000);
	while (retry--) {
		ret = i2c_master_recv(es305->client, recv, 4);
		if (ret < 0) {
			dev_err(es305->dev, "i2c_master_recv failed (%d)\n", ret);
			return ret;
		}
		/*
		 * Check that the first two bytes of the response match
		 * (the ack is in those bytes)
		 */
		if ((send[0] == recv[0]) && (send[1] == recv[1])) {
			if (response)
				*response = (recv[2] << 8) | recv[3];
			ret = 0;
			break;
		} else {
			dev_err(es305->dev, "incorrect ack (got 0x%.2x%.2x)\n",
				recv[0], recv[1]);
			ret = -EINVAL;
		}

		/* Wait before polling again */
		if (retry > 0)
			msleep(20);
	}

	return ret;
}

static int es305_load_firmware(struct es305_data *es305)
{
	int ret = 0;
	const u8 *i2c_cmds;
	int size;

	i2c_cmds = es305->fw->data;
	size = es305->fw->size;

	while (size > 0) {
		ret = i2c_master_send(es305->client, i2c_cmds,
				min(size, MAX_CMD_SEND_SIZE));
		if (ret < 0) {
			dev_err(es305->dev, "i2c_master_send failed (%d)\n", ret);
			break;
		}
		size -= MAX_CMD_SEND_SIZE;
		i2c_cmds += MAX_CMD_SEND_SIZE;
	}

	return ret;
}

static int es305_reset(struct es305_data *es305)
{
	int ret = 0;
	static const u8 boot[2] = {ES305_BOOT >> 8, ES305_BOOT};
	u8 ack;
	int retry = RETRY_COUNT;

	while (retry--) {
		/* Reset ES305 chip */
		gpiod_set_value(es305->gpio_reset, 0);
		usleep_range(200, 400);
		gpiod_set_value(es305->gpio_reset, 1);

		/* Delay before sending i2c commands */
		msleep(50);

		/*
		 * Send boot command and check response. The boot command
		 * is different from the others in that it's only 2 bytes,
		 * and the ack retry mechanism is different too.
		 */
		ret = i2c_master_send(es305->client, boot, 2);
		if (ret < 0) {
			dev_err(es305->dev, "i2c_master_send failed (%d)\n", ret);
			continue;
		}
		usleep_range(1000, 2000);
		ret = i2c_master_recv(es305->client, &ack, 1);
		if (ret < 0) {
			dev_err(es305->dev, "i2c_master_recv failed (%d)\n", ret);
			continue;
		}
		if (ack != ES305_BOOT_ACK) {
			dev_err(es305->dev, "boot ack incorrect (got 0x%.2x)\n", ack);
			continue;
		}

		ret = es305_load_firmware(es305);
		if (ret < 0) {
			dev_err(es305->dev, "load firmware error (%d)\n", ret);
			continue;
		}

		/* Delay before issuing a sync command */
		msleep(120);

		ret = es305_send_cmd(es305, ES305_SYNC_POLLING, NULL);
		if (ret < 0) {
			dev_err(es305->dev, "sync error (%d)\n", ret);
			continue;
		}

		break;
	}

	return ret;
}

static int es305_sleep(struct es305_data *es305)
{
	int ret = 0;

	if (es305->asleep)
		return ret;

	ret = es305_send_cmd(es305, ES305_SET_POWER_STATE_SLEEP, NULL);
	if (ret < 0) {
		dev_err(es305->dev, "set power state error (%d)\n", ret);
		return ret;
	}

	/* The clock can be disabled after the device has had time to sleep */
	msleep(20);
	clk_disable(es305->mclk);
	gpiod_set_value(es305->gpio_wakeup, 1);

	es305->asleep = true;

	return ret;
}

static int es305_wake(struct es305_data *es305)
{
	int ret = 0;

	if (!es305->asleep)
		return ret;

	clk_enable(es305->mclk);
	gpiod_set_value(es305->gpio_wakeup, 0);
	msleep(30);

	ret = es305_send_cmd(es305, ES305_SYNC_POLLING, NULL);
	if (ret < 0) {
		dev_err(es305->dev, "sync error (%d)\n", ret);

		/* Go back to sleep */
		clk_disable(es305->mclk);
		gpiod_set_value(es305->gpio_wakeup, 1);
		return ret;
	}

	es305->asleep = false;

	return ret;
}

static int es305_set_passthrough(struct es305_data *es305)
{
	int ret = 0;
	u32 path;

	if (!es305->device_ready) {
		dev_warn(es305->dev, "Device not ready.\n");
		return -EAGAIN;
	}

	switch (es305->passthrough) {
	case 0:
		return 0;
	case 1:
		path = 0x44;
		break;
	case 2:
		path = 0x48;
		break;
	case 3:
		path = 0x4c;
		break;
	case 4:
		path = 0x58;
		break;
	case 5:
		path = 0x5c;
		break;
	case 6:
		path = 0x6c;
		break;
	default:
		return -EINVAL;
	}

	ret = es305_wake(es305);
	if (ret < 0)
		return ret;

	ret = es305_send_cmd(es305, ES305_DIGITAL_PASSTHROUGH | path, NULL);
	if (ret < 0) {
		dev_err(es305->dev, "set passthrough error (%d)\n", ret);
		return ret;
	}

	return ret;
}

static void es305_firmware_ready(const struct firmware *fw, void *context)
{
	struct es305_data *es305 = (struct es305_data *)context;
	int ret;

	if (!fw) {
		dev_err(es305->dev, "firmware request failed\n");
		return;
	}
	es305->fw = fw;

	clk_enable(es305->mclk);

	ret = es305_reset(es305);
	if (ret < 0)
		goto err;

	es305->device_ready = true;

	ret = es305_set_passthrough(es305);
	if (ret < 0)
		goto err;

	ret = es305_sleep(es305);
	if (ret < 0)
		goto err;

	return;

err:
	release_firmware(es305->fw);
	es305->fw = NULL;
	es305->device_ready = false;
}

static const char * const es305_passthrough_texts[] = {
	"None",
	"A-B",
	"A-C",
	"A-D",
	"B-C",
	"B-D",
	"C-D",
};
static SOC_ENUM_SINGLE_EXT_DECL(es305_passthrough_enum, es305_passthrough_texts);

static int es305_passthrough_control_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct es305_data *es305 = snd_soc_component_get_drvdata(c);

	ucontrol->value.enumerated.item[0] = es305->passthrough;

	return 0;
}

static int es305_passthrough_control_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct es305_data *es305 = snd_soc_component_get_drvdata(c);
	int ret;

	if (ucontrol->value.enumerated.item[0] > e->items)
		return -EINVAL;

	if (es305->passthrough == ucontrol->value.enumerated.item[0])
		return 0;

	es305->passthrough = ucontrol->value.enumerated.item[0];

	ret = es305_set_passthrough(es305);
	/*
	 * Ignore EAGAIN as passthrough will then be set once the firmware will
	 * be ready
	 */
	if ((ret < 0) && (ret != -EAGAIN))
		return ret;

	return 0;
}

static const struct snd_kcontrol_new es305_passthrough =
	SOC_DAPM_ENUM_EXT("Passthrough",
			  es305_passthrough_enum,
			  es305_passthrough_control_get,
			  es305_passthrough_control_put);

static unsigned int es305_read(struct snd_soc_component *component,
				    unsigned int reg)
{
	struct es305_data *es305 = snd_soc_component_get_drvdata(component);

	return es305->passthrough;
}

static int es305_passthrough_event(struct snd_soc_dapm_widget *w,
			    struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *c = snd_soc_dapm_to_component(w->dapm);
	struct es305_data *es305 = snd_soc_component_get_drvdata(c);
	int ret;

	ret = es305_set_passthrough(es305);
	/*
	 * Ignore EAGAIN as passthrough will then be set once the firmware will
	 * be ready
	 */
	if ((ret < 0) && (ret != -EAGAIN))
		return ret;

	return 0;
}

static const struct snd_soc_dapm_widget es305_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("Port A IN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("Port B IN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("Port C IN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("Port D IN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MUX_E("MUX", SND_SOC_NOPM, 0, 0,
			   &es305_passthrough, es305_passthrough_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_OUT("Port A OUT", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("Port B OUT", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("Port C OUT", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("Port D OUT", NULL, 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route es305_dapm_routes[] = {
	{ "Port A OUT", NULL, "MUX" },
	{ "Port B OUT", NULL, "MUX" },
	{ "Port C OUT", NULL, "MUX" },
	{ "Port D OUT", NULL, "MUX" },
	{ "MUX", "A-B", "Port A IN" },
	{ "MUX", "A-B", "Port B IN" },
	{ "MUX", "A-C", "Port A IN" },
	{ "MUX", "A-C", "Port C IN" },
	{ "MUX", "A-D", "Port A IN" },
	{ "MUX", "A-D", "Port D IN" },
	{ "MUX", "B-C", "Port B IN" },
	{ "MUX", "B-C", "Port C IN" },
	{ "MUX", "B-D", "Port B IN" },
	{ "MUX", "B-D", "Port D IN" },
	{ "MUX", "C-D", "Port C IN" },
	{ "MUX", "C-D", "Port D IN" },
};

#define ES305_RATES (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |\
			SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 |\
			SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000 |\
			SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)

#define ES305_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S16_BE |\
			SNDRV_PCM_FMTBIT_S20_3LE | SNDRV_PCM_FMTBIT_S20_3BE |\
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_BE |\
			SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S32_BE)

static struct snd_soc_dai_driver es305_dai[] = {
	{
		.name = "es305-a",
		.id = 1,
		.playback = {
			.stream_name = "Port A Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
			.rates = ES305_RATES,
			.formats = ES305_FORMATS,
		},
		.capture = {
			.stream_name = "Port A Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
			.rates = ES305_RATES,
			.formats = ES305_FORMATS,
		 },
	},
	{
		.name = "es305-b",
		.id = 2,
		.playback = {
			.stream_name = "Port B Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
			.rates = ES305_RATES,
			.formats = ES305_FORMATS,
		},
		.capture = {
			.stream_name = "Port B Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
			.rates = ES305_RATES,
			.formats = ES305_FORMATS,
		 },
	},
	{
		.name = "es305-c",
		.id = 3,
		.playback = {
			.stream_name = "Port C Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
			.rates = ES305_RATES,
			.formats = ES305_FORMATS,
		},
		.capture = {
			.stream_name = "Port C Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
			.rates = ES305_RATES,
			.formats = ES305_FORMATS,
		 },
	},
	{
		.name = "es305-d",
		.id = 4,
		.playback = {
			.stream_name = "Port D Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
			.rates = ES305_RATES,
			.formats = ES305_FORMATS,
		},
		.capture = {
			.stream_name = "Port D Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 192000,
			.rates = ES305_RATES,
			.formats = ES305_FORMATS,
		 },
	},
};

static const struct snd_soc_component_driver soc_component_es305 = {
	.name			= "es305",
	.dapm_widgets		= es305_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(es305_dapm_widgets),
	.dapm_routes		= es305_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(es305_dapm_routes),
	.read			= es305_read,
};

static int es305_probe(struct i2c_client *client)
{
	struct es305_data *es305;
	int ret = 0;

	es305 = devm_kzalloc(&client->dev, sizeof(*es305), GFP_KERNEL);
	if (!es305)
		return -ENOMEM;

	es305->client = client;
	i2c_set_clientdata(client, es305);

	es305->dev = &client->dev;

	es305->gpio_wakeup = devm_gpiod_get(es305->dev, "device-wakeup", GPIOD_OUT_HIGH);
	if (IS_ERR(es305->gpio_wakeup)) {
		ret = PTR_ERR(es305->gpio_wakeup);
		dev_err(es305->dev, "error requesting wakeup gpio (%d)\n", ret);
		return ret;
	}
	gpiod_set_value(es305->gpio_wakeup, 0);

	es305->gpio_reset = devm_gpiod_get(es305->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(es305->gpio_reset)) {
		ret = PTR_ERR(es305->gpio_reset);
		dev_err(es305->dev, "error requesting reset gpio (%d)\n", ret);
		return ret;
	}
	gpiod_set_value(es305->gpio_reset, 0);

	es305->mclk = devm_clk_get(es305->dev, "MCLK");
	if (IS_ERR(es305->mclk)) {
		ret = PTR_ERR(es305->mclk);
		dev_err(es305->dev, "Failed to get clock MCLK (%d)\n", ret);
		return ret;
	}
	ret = clk_prepare_enable(es305->mclk);
	if (ret < 0) {
		dev_err(es305->dev, "Failed to enable clock MCLK (%d)\n", ret);
		return ret;
	}

	request_firmware_nowait(THIS_MODULE, FW_ACTION_UEVENT,
				FIRMWARE_ES305, es305->dev, GFP_KERNEL,
				es305, es305_firmware_ready);

	ret = devm_snd_soc_register_component(es305->dev, &soc_component_es305,
				es305_dai, ARRAY_SIZE(es305_dai));
	if (ret < 0) {
		dev_err(es305->dev, "Failed to register SOC component (%d)\n", ret);
		clk_disable_unprepare(es305->mclk);
		return ret;
	}

	return 0;
}

static void es305_remove(struct i2c_client *client)
{
	struct es305_data *es305 = i2c_get_clientdata(client);

	es305->device_ready = false;

	release_firmware(es305->fw);
	es305->fw = NULL;

	clk_disable_unprepare(es305->mclk);
}

static const struct i2c_device_id es305_id[] = {
	{ "audience_es305", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, es305_id);

static struct of_device_id es305_match_table[] = {
	{ .compatible = "audience,es305" },
	{ },
};
MODULE_DEVICE_TABLE(of, es305_match_table);

static struct i2c_driver es305_driver = {
	.driver = {
		.name = "es305",
		.of_match_table = of_match_ptr(es305_match_table),
	},
	.probe = es305_probe,
	.remove = es305_remove,
	.id_table = es305_id,
};
module_i2c_driver(es305_driver);

MODULE_DESCRIPTION("Audience ES305 Voice Processor driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(FIRMWARE_ES305);
