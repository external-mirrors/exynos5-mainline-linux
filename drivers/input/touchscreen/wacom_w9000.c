// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Wacom W9000-series penabled I2C touchscreen driver
 *
 * Copyright (c) 2025 Hendrik Noack
 */

#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/property.h>
#include <linux/delay.h>
#include <uapi/linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/unaligned.h>
#include <linux/minmax.h>

/* Wacom command */
#define COM_COORD_NUM_MAX		12
#define COM_QUERY_NUM_MAX		11

// Commands
#define COM_SAMPLERATE_STOP	0x30
#define COM_SAMPLERATE_40	0x33
#define COM_SAMPLERATE_80	0x32
#define COM_SAMPLERATE_133	0x31
#define COM_QUERY		0x2a
#define COM_SURVEYSCAN		0x2b
#define COM_CHECKSUM		0x63

struct wacom_features {
	unsigned int x_max;
	unsigned int y_max;
	unsigned int pressure_max;
	unsigned int fw_version;
};

struct wacom_w9000_variant {
	int com_coord_num;
	int com_query_num;
	char *name;
};

struct wacom_w9000_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct wacom_features *features;
	const struct wacom_w9000_variant *variant;
	unsigned int irq;
	unsigned int pen_detect_irq;
	unsigned int pen_insert_irq;
	bool x_invert;
	bool y_invert;
	bool xy_swap;
	bool pen_inserted;
	bool pen_detect;
	bool pen_proximity;
	int tool;
	struct regulator *regulator;
	struct gpio_desc *flash_mode_gpio;
	struct gpio_desc *pen_detect_gpio;
	struct gpio_desc *pen_inserted_gpio;
};

static const struct wacom_w9000_variant wacom_w9007a_lt03 = {
	.com_coord_num	= 8,
	.com_query_num	= 9,
	.name = "Wacom W9007 LT03 Digitizer",
};

static const struct wacom_w9000_variant wacom_w9007a_v1 = {
	.com_coord_num	= 12,
	.com_query_num	= 9,
	.name = "Wacom W9007 V1 Digitizer",
};

static const struct wacom_w9000_variant wacom_w9010 = {
	.com_coord_num	= 12,
	.com_query_num	= 11,
	.name = "Wacom W9010 Digitizer",
};

static const struct of_device_id wacom_w9000_of_match[] = {
	{ .compatible = "wacom,w9007a_lt03", .data=&wacom_w9007a_lt03, },
	{ .compatible = "wacom,w9007a_v1", .data=&wacom_w9007a_v1, },
	{ .compatible = "wacom,w9010", .data=&wacom_w9010, },
	{},
};
MODULE_DEVICE_TABLE(of, wacom_w9000_of_match);

static int wacom_w9000_read(struct i2c_client *client, u8 command, int len, char *data)
{
	struct i2c_msg xfer[2];
	bool retried = false;
	int ret;

	/* Write register */
	xfer[0].addr = client->addr;
	xfer[0].flags = 0;
	xfer[0].len = 1;
	xfer[0].buf = &command;

	/* Read data */
	xfer[1].addr = client->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = len;
	xfer[1].buf = data;

retry:
	ret = i2c_transfer(client->adapter, xfer, 2);
	if (ret == 2) {
		ret = 0;
	} else if (!retried) {
		retried = true;
		goto retry;
	} else {
		if (ret >= 0)
			ret = -EIO;
		dev_err(&client->dev, "%s: i2c transfer failed (%d)\n", __func__, ret);
	}

	return ret;
}

static int wacom_w9000_query(struct wacom_w9000_data *wacom_data)
{
	struct i2c_client *client = wacom_data->client;
	struct device *dev = &wacom_data->client->dev;
	bool retried = false;
	int ret;
	u8 data[COM_QUERY_NUM_MAX];

retry:
	ret = wacom_w9000_read(client, COM_QUERY, wacom_data->variant->com_query_num, data);
	if (ret)
		return ret;

	if (data[0] == 0x00 && !retried){
		retried = true;
		goto retry;
	} else if (data[0] == 0x0f)
		wacom_data->features->fw_version = get_unaligned_be16(&data[7]);

	dev_dbg(dev, "query: %X, %X, %X, %X, %X, %X, %X, %X, %X, %d\n", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], retried);

	if (wacom_data->xy_swap) {
		wacom_data->features->x_max = get_unaligned_be16(&data[3]);
		wacom_data->features->y_max = get_unaligned_be16(&data[1]);
	} else {
		wacom_data->features->x_max = get_unaligned_be16(&data[1]);
		wacom_data->features->y_max = get_unaligned_be16(&data[3]);
	}
	wacom_data->features->pressure_max = get_unaligned_be16(&data[5]);

	dev_dbg(dev, "x_max:%d, y_max:%d, pressure_max:%d, fw:0x%X", wacom_data->features->x_max, wacom_data->features->y_max, wacom_data->features->pressure_max, wacom_data->features->fw_version);

	return 0;
}

static int wacom_w9000_coord(struct wacom_w9000_data *wacom_data)
{
	struct i2c_client *client = wacom_data->client;
	struct device *dev = &wacom_data->client->dev;
	int ret;
	u8 data[COM_COORD_NUM_MAX];
	bool touch, rubber, side_button;
	u16 x, y, pressure;
	u8 distance;

	ret = i2c_master_recv(client, data, wacom_data->variant->com_coord_num);
	if (ret != wacom_data->variant->com_coord_num) {
		if (ret >= 0)
			ret = -EIO;
		dev_err(dev, "%s: i2c receive failed (%d)\n", __func__, ret);
	}

	dev_dbg(dev, "data: %X, %X, %X, %X, %X, %X, %X, %X, %X, %X, %X, %X", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11]);

	if (data[0] & BIT(7)) {
		wacom_data->pen_proximity = 1;

		touch = !!(data[0] & BIT(4));
		side_button = !!(data[0] & BIT(5));
		rubber = !!(data[0] & BIT(6));

		x = get_unaligned_be16(&data[1]);
		y = get_unaligned_be16(&data[3]);
		pressure = get_unaligned_be16(&data[5]);
		distance = data[7];

		if (wacom_data->x_invert)
			x = wacom_data->features->x_max - x;
		if (wacom_data->y_invert)
			y = wacom_data->features->y_max - y;
		if (wacom_data->xy_swap) {
			swap(x, y);
		}

		if (!((x <= wacom_data->features->x_max) && (y <= wacom_data->features->y_max))) {
			dev_warn(dev, "Coordinates out of range x=%d, y=%d", x, y);
			return 0;
		}

		input_report_abs(wacom_data->input_dev, ABS_X, x);
		input_report_abs(wacom_data->input_dev, ABS_Y, y);
		input_report_abs(wacom_data->input_dev, ABS_PRESSURE, pressure);
		input_report_abs(wacom_data->input_dev, ABS_DISTANCE, distance);
		input_report_key(wacom_data->input_dev, BTN_STYLUS, side_button);
		input_report_key(wacom_data->input_dev, BTN_TOUCH, touch);
		input_report_key(wacom_data->input_dev, BTN_TOOL_PEN, !rubber);
		input_report_key(wacom_data->input_dev, BTN_TOOL_RUBBER, rubber);
		input_sync(wacom_data->input_dev);
	} else {
		if (wacom_data->pen_proximity) {
			input_report_abs(wacom_data->input_dev, ABS_PRESSURE, 0);
			input_report_abs(wacom_data->input_dev, ABS_DISTANCE, 0);
			input_report_key(wacom_data->input_dev, BTN_STYLUS, 0);
			input_report_key(wacom_data->input_dev, BTN_TOUCH, 0);
			input_report_key(wacom_data->input_dev, BTN_TOOL_PEN, 0);
			input_report_key(wacom_data->input_dev, BTN_TOOL_RUBBER, 0);
			input_sync(wacom_data->input_dev);

			wacom_data->pen_proximity = 0;
		}
	}

	return 0;
}

static irqreturn_t wacom_w9000_interrupt(int irq, void *dev_id)
{
	struct wacom_w9000_data *wacom_data = dev_id;

	wacom_w9000_coord(wacom_data);

	return IRQ_HANDLED;
}

static irqreturn_t wacom_w9000_interrupt_pen_detect(int irq, void *dev_id)
{
	struct wacom_w9000_data *wacom_data = dev_id;

	wacom_data->pen_detect = gpiod_get_value(wacom_data->pen_detect_gpio);

	return IRQ_HANDLED;
}

static irqreturn_t wacom_w9000_interrupt_pen_insert(int irq, void *dev_id)
{
	struct wacom_w9000_data *wacom_data = dev_id;
	struct device *dev = &wacom_data->client->dev;

	wacom_data->pen_inserted = gpiod_get_value(wacom_data->pen_inserted_gpio);

	input_report_switch(wacom_data->input_dev, SW_PEN_INSERTED, wacom_data->pen_inserted);
	input_sync(wacom_data->input_dev);

	if (!wacom_data->pen_inserted && !regulator_is_enabled(wacom_data->regulator)) {
		int error = regulator_enable(wacom_data->regulator);
		if (error) {
			dev_err(dev, "Failed to enable regulators: %d\n", error);
			return error;
		}
		msleep(200);
		enable_irq(wacom_data->irq);
	} else if (wacom_data->pen_inserted && regulator_is_enabled(wacom_data->regulator)) {
		disable_irq(wacom_data->irq);
		regulator_disable(wacom_data->regulator);
	}

	dev_dbg(dev, "Pen inserted changed to %d", wacom_data->pen_inserted);

	return IRQ_HANDLED;
}

static int wacom_w9000_open(struct input_dev *dev)
{
	struct wacom_w9000_data *wacom_data = input_get_drvdata(dev);

	if (!wacom_data->pen_inserted && !regulator_is_enabled(wacom_data->regulator)) {
		int error = regulator_enable(wacom_data->regulator);
		if (error) {
			dev_err(&wacom_data->client->dev, "Failed to enable regulators: %d\n", error);
			return error;
		}
		msleep(200);
		enable_irq(wacom_data->irq);
	}
	return 0;
}

static void wacom_w9000_close(struct input_dev *dev)
{
	struct wacom_w9000_data *wacom_data = input_get_drvdata(dev);

	if (regulator_is_enabled(wacom_data->regulator)) {
		disable_irq(wacom_data->irq);
		regulator_disable(wacom_data->regulator);
	}
}

static int wacom_w9000_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct wacom_w9000_data *wacom_data;
	struct input_dev *input_dev;
	int error;
	u32 val;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "i2c_check_functionality error\n");
		return -EIO;
	}

	wacom_data = devm_kzalloc(dev, sizeof(struct wacom_w9000_data), GFP_KERNEL);
	if (!wacom_data)
		return -ENOMEM;

	wacom_data->features = devm_kzalloc(dev, sizeof(struct wacom_features), GFP_KERNEL);
	if (!wacom_data->features)
		return -ENOMEM;

	wacom_data->variant = i2c_get_match_data(client);

	wacom_data->client = client;

	input_dev = devm_input_allocate_device(dev);
	if (!input_dev)
		return -ENOMEM;
	wacom_data->input_dev = input_dev;

	wacom_data->irq = client->irq;
	i2c_set_clientdata(client, wacom_data);

	wacom_data->regulator = devm_regulator_get(dev, "vdd");
	if (IS_ERR(wacom_data->regulator)) {
		error = PTR_ERR(wacom_data->regulator);
		dev_err(dev, "Failed to get regulators %d\n", error);
		return error;
	}

	/* Request flash-mode line and don't go into flash mode */
	wacom_data->flash_mode_gpio = devm_gpiod_get_optional(dev, "flash-mode", GPIOD_OUT_LOW);
	if (IS_ERR(wacom_data->flash_mode_gpio)) {
		error = PTR_ERR(wacom_data->flash_mode_gpio);
		dev_err(dev, "Failed to get flash-mode gpio: %d\n", error);
		return error;
	}

	/* Request pdct line  */
	wacom_data->pen_detect_gpio = devm_gpiod_get_optional(dev, "pdct", GPIOD_IN);
	if (IS_ERR(wacom_data->pen_detect_gpio)) {
		error = PTR_ERR(wacom_data->pen_detect_gpio);
		dev_err(dev, "Failed to get pdct gpio: %d\n", error);
		return error;
	}

	/* Request pen-insert line  */
	wacom_data->pen_inserted_gpio = devm_gpiod_get_optional(dev, "pen-inserted", GPIOD_IN);
	if (IS_ERR(wacom_data->pen_inserted_gpio)) {
		error = PTR_ERR(wacom_data->pen_inserted_gpio);
		dev_err(dev, "Failed to get pen-insert gpio: %d\n", error);
		return error;
	}

	error = regulator_enable(wacom_data->regulator);
	if (error) {
		dev_err(dev, "Failed to enable regulators: %d\n", error);
		return error;
	}

	msleep(200);

	wacom_data->x_invert = device_property_read_bool(dev, "touchscreen-inverted-x");
	wacom_data->y_invert = device_property_read_bool(dev, "touchscreen-inverted-y");
	wacom_data->xy_swap = device_property_read_bool(dev, "touchscreen-swapped-x-y");

	error = wacom_w9000_query(wacom_data);
	if (error)
		goto err_disable_regulators;

	input_dev->name = wacom_data->variant->name;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = dev;
	input_dev->id.vendor = 0x56a;
	input_dev->id.version = wacom_data->features->fw_version;
	input_dev->open = wacom_w9000_open;
	input_dev->close = wacom_w9000_close;
	input_dev->evbit[0] |= BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

	// Calculate x and y resolution from size in devicetree
	error = device_property_read_u32(dev, "touchscreen-x-mm", &val);
	if (error)
		input_abs_set_res(input_dev, ABS_X, 100);
	else
		input_abs_set_res(input_dev, ABS_X, wacom_data->features->x_max / val);
	error = device_property_read_u32(dev, "touchscreen-y-mm", &val);
	if (error)
		input_abs_set_res(input_dev, ABS_Y, 100);
	else
		input_abs_set_res(input_dev, ABS_Y, wacom_data->features->y_max / val);

	input_set_abs_params(input_dev, ABS_X, 0, wacom_data->features->x_max, 4, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, wacom_data->features->y_max, 4, 0);
	input_set_abs_params(input_dev, ABS_PRESSURE, 0, wacom_data->features->pressure_max, 0, 0);
	input_set_abs_params(input_dev, ABS_DISTANCE, 0, 255, 0, 0);

	__set_bit(BTN_TOUCH, input_dev->keybit);
	__set_bit(BTN_TOOL_PEN, input_dev->keybit);
	__set_bit(BTN_TOOL_RUBBER, input_dev->keybit);
	__set_bit(BTN_STYLUS, input_dev->keybit);

	error = devm_request_threaded_irq(dev, wacom_data->irq, NULL, wacom_w9000_interrupt, IRQF_ONESHOT | IRQF_NO_AUTOEN, client->name, wacom_data);
	if (error) {
		dev_err(dev, "Failed to register interrupt\n");
		goto err_disable_regulators;
	}

	if (wacom_data->pen_detect_gpio) {
		wacom_data->pen_detect_irq = gpiod_to_irq(wacom_data->pen_detect_gpio);
		error = devm_request_threaded_irq(dev, wacom_data->pen_detect_irq, NULL, wacom_w9000_interrupt_pen_detect, IRQF_ONESHOT | IRQF_NO_AUTOEN | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "wacom_pdct", wacom_data);
		if (error) {
			dev_err(dev, "Failed to register pdct interrupt\n");
			goto err_disable_regulators;
		}
	}

	if (wacom_data->pen_inserted_gpio) {
		input_dev->evbit[0] |= BIT_MASK(EV_SW);
		input_set_capability(input_dev, EV_SW, SW_PEN_INSERTED);
		wacom_data->pen_insert_irq = gpiod_to_irq(wacom_data->pen_inserted_gpio);
		error = devm_request_threaded_irq(dev, wacom_data->pen_insert_irq, NULL, wacom_w9000_interrupt_pen_insert, IRQF_ONESHOT | IRQF_NO_AUTOEN | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "wacom_pen_insert", wacom_data);
		if (error) {
			dev_err(dev, "Failed to register pen-insert interrupt\n");
			goto err_disable_regulators;
		}
	}

	input_set_drvdata(input_dev, wacom_data);

	wacom_data->pen_inserted = gpiod_get_value(wacom_data->pen_inserted_gpio);
	if (wacom_data->pen_inserted)
		regulator_disable(wacom_data->regulator);
	else
		enable_irq(wacom_data->irq);

	input_report_switch(wacom_data->input_dev, SW_PEN_INSERTED, wacom_data->pen_inserted);
	input_sync(wacom_data->input_dev);

	if (wacom_data->pen_inserted_gpio) {
		enable_irq(wacom_data->pen_insert_irq);
	}

	if (wacom_data->pen_detect_gpio)
		enable_irq(wacom_data->pen_detect_irq);

	error = input_register_device(wacom_data->input_dev);
	if (error) {
		dev_err(dev, "Failed to register input device: %d\n", error);
		goto err_disable_regulators;
	}

	return 0;

err_disable_regulators:
	regulator_disable(wacom_data->regulator);
	return error;
}

static void wacom_w9000_remove(struct i2c_client *client)
{
	struct wacom_w9000_data *wacom_data = i2c_get_clientdata(client);

	if (regulator_is_enabled(wacom_data->regulator))
		regulator_disable(wacom_data->regulator);
}

static int wacom_w9000_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct wacom_w9000_data *wacom_data = i2c_get_clientdata(client);
	struct input_dev *input_dev = wacom_data->input_dev;

	if (!input_dev)
		return 0;

	mutex_lock(&input_dev->mutex);

	if (input_device_enabled(input_dev) && regulator_is_enabled(wacom_data->regulator)) {
		disable_irq(wacom_data->irq);
		regulator_disable(wacom_data->regulator);
	}

	mutex_unlock(&input_dev->mutex);

	return 0;
}

static int wacom_w9000_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct wacom_w9000_data *wacom_data = i2c_get_clientdata(client);
	struct input_dev *input_dev = wacom_data->input_dev;

	if (!input_dev)
		return 0;

	mutex_lock(&input_dev->mutex);

	if (input_device_enabled(input_dev) && !wacom_data->pen_inserted && !regulator_is_enabled(wacom_data->regulator)) {
		int error = regulator_enable(wacom_data->regulator);
		if (error) {
			dev_err(&wacom_data->client->dev, "Failed to enable regulators: %d\n", error);
			return error;
		}
		msleep(200);
		enable_irq(wacom_data->irq);
	}

	mutex_unlock(&input_dev->mutex);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(wacom_w9000_pm, wacom_w9000_suspend, wacom_w9000_resume);

static const struct i2c_device_id wacom_w9000_id[] = {
	{ "w9007a_lt03" },
	{ "w9007a_v1" },
	{ "w9010" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, wacom_w9000_id);

static struct i2c_driver wacom_w9000_driver = {
	.driver = {
		.name	= "wacom_w9000",
		.of_match_table = wacom_w9000_of_match,
		.pm	= pm_sleep_ptr(&wacom_w9000_pm),
	},
	.probe		= wacom_w9000_probe,
	.remove		= wacom_w9000_remove,
	.id_table	= wacom_w9000_id,
};
module_i2c_driver(wacom_w9000_driver);

/* Module information */
MODULE_AUTHOR("Hendrik Noack <hendrik.noack@protonmail.com>");
MODULE_DESCRIPTION("Wacom W9000-series penabled touchscreen driver");
MODULE_LICENSE("GPL");
