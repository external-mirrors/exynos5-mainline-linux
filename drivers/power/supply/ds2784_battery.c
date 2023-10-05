#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/w1.h>
#include <linux/of.h>

#include "../../w1/slaves/w1_ds2784.h"

static const char model[] = "DS2784";
static const char manufacturer[] = "Maxim/Dallas";

struct ds2784_device_info {
	struct device *dev;

	/* DS2784 data, valid after calling ds2784_battery_read_status() */
	char raw[DS2784_DATA_SIZE];
	int voltage_uV;
	int temp_C;
	int current_uA;
	int current_avg_uA;
	int rem_capacity;
	int charge_cnt;
	bool inited;

	struct power_supply *bat;
	struct power_supply_desc bat_desc;
	struct device *w1_dev;
};

static inline int ds2784_battery_io(struct ds2784_device_info *di,
	char *buf, int addr, size_t count, int io)
{
	return w1_ds2784_io(di->w1_dev, buf, addr, count, io);
}

static int w1_ds2784_read(struct ds2784_device_info *di, char *buf,
		int addr, size_t count)
{
	return ds2784_battery_io(di, buf, addr, count, 0);
}

static int ds2784_get_soc(struct ds2784_device_info *di, int *soc)
{
	int ret;

	ret = w1_ds2784_read(di, di->raw + DS2784_REG_RARC, DS2784_REG_RARC, 1);

	if (ret < 0)
		return ret;

	*soc = di->raw[DS2784_REG_RARC];
	return 0;
}

static int ds2784_get_vcell(struct ds2784_device_info *di, int *vcell)
{
	int ret;
	short n;

	ret = w1_ds2784_read(di, di->raw + DS2784_REG_VOLT_MSB,
			DS2784_REG_VOLT_MSB, 2);

	if (ret < 0)
		return ret;

	n = (((di->raw[DS2784_REG_VOLT_MSB] << 8) |
				(di->raw[DS2784_REG_VOLT_LSB])) >> 5);
	*vcell = n * 4886;
	return 0;
}

static int ds2784_get_current(struct ds2784_device_info *di, bool avg, int *ival)
{
	int reg = avg ? DS2784_REG_AVG_CURR_MSB : DS2784_REG_CURR_MSB;
	short n;
	int ret;
	int div_rsnsp;

	if (!di->raw[DS2784_REG_RSNSP]) {
		ret = w1_ds2784_read(di, di->raw + DS2784_REG_RSNSP,
				DS2784_REG_RSNSP, 1);
		if (ret < 0)
			dev_err(di->dev, "error %d reading RSNSP\n", ret);
	}
	div_rsnsp = 10000 / di->raw[DS2784_REG_RSNSP];

	ret = w1_ds2784_read(di, di->raw + reg, reg, 2);
	if (ret < 0)
		return ret;

	n = ((di->raw[reg] << 8) | (di->raw[reg+1]));

	*ival = div_s64((long long)n * 15625, div_rsnsp);
	return 0;
}

static int ds2784_get_current_now(struct ds2784_device_info *di, int *i_current)
{
	return ds2784_get_current(di, false, i_current);
}

static int ds2784_get_current_avg(struct ds2784_device_info *di, int *i_avg)
{
	return ds2784_get_current(di, true, i_avg);
}

static int ds2784_get_temperature(struct ds2784_device_info *di, int *temp_now)
{
	short n;
	int ret;

	ret = w1_ds2784_read(di, di->raw + DS2784_REG_TEMP_MSB,
			DS2784_REG_TEMP_MSB, 2);

	if (ret < 0)
		return ret;

	n = (((di->raw[DS2784_REG_TEMP_MSB] << 8) |
			(di->raw[DS2784_REG_TEMP_LSB])) >> 5);

	if (di->raw[DS2784_REG_TEMP_MSB] & (1 << 7))
		n |= 0xf800;

	*temp_now = (n * 10) / 8;
	return 0;
}

static int ds2784_get_accumulated_current(struct ds2784_device_info *di, int *acc)
{
	int n;
	int ret;
	int div_rsnsp;

	if (!di->raw[DS2784_REG_RSNSP]) {
		ret = w1_ds2784_read(di, di->raw + DS2784_REG_RSNSP,
				DS2784_REG_RSNSP, 1);
		if (ret < 0) {
			dev_err(di->dev, "error %d reading RSNSP\n", ret);
			return ret;
		}
	}
	div_rsnsp = 100 / di->raw[DS2784_REG_RSNSP];

	ret = w1_ds2784_read(di, di->raw + DS2784_REG_ACCUMULATE_CURR_MSB,
			DS2784_REG_ACCUMULATE_CURR_MSB, 2);

	if (ret < 0)
		return ret;

	n = (di->raw[DS2784_REG_ACCUMULATE_CURR_MSB] << 8) |
		di->raw[DS2784_REG_ACCUMULATE_CURR_LSB];
	*acc = n * 625 / div_rsnsp;

	return 0;
}

static int ds2784_battery_read_status(struct ds2784_device_info *di)
{
	int ret = 0;

	if (!di->inited)
		return -ENODEV;

	ret = ds2784_get_vcell(di, &di->voltage_uV);
	if (ret < 0)
		goto out;

	ret = ds2784_get_temperature(di, &di->temp_C);
	if (ret < 0)
		goto out;

	ret = ds2784_get_current_now(di, &di->current_uA);
	if (ret < 0)
		goto out;

	ret = ds2784_get_current_avg(di, &di->current_avg_uA);
	if (ret < 0)
		goto out;

	ret = ds2784_get_soc(di, &di->rem_capacity);
	if (ret < 0)
		goto out;

	ret = ds2784_get_accumulated_current(di, &di->charge_cnt);

out:
	return ret;
}

static int ds2784_get_property(struct power_supply *psy,
	enum power_supply_property psp,
	union power_supply_propval *val)
{
	int ret = 0;
	struct ds2784_device_info *di = power_supply_get_drvdata(psy);

	if (!di->inited)
		return -ENODEV;

	ret = ds2784_battery_read_status(di);

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = di->voltage_uV;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = di->temp_C;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = model;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = manufacturer;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = di->current_uA;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = di->current_avg_uA;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = di->rem_capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = di->charge_cnt;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static enum power_supply_property ds2784_props[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
};

static int ds2784_battery_probe(struct platform_device *pdev)
{
	struct power_supply_config psy_cfg = {};
	struct ds2784_device_info *di;
	struct device *dev = &pdev->dev;
	int retval = 0;

	di = devm_kzalloc(dev, sizeof(*di), GFP_KERNEL);
	if (!di) {
		retval = -ENOMEM;
		goto out;
	}

	platform_set_drvdata(pdev, di);

	di->dev						= dev;
	di->w1_dev					= dev->parent;
	di->bat_desc.name 			= devm_kasprintf(dev, GFP_KERNEL, "ds2784-battery.%d", dev->id);
	di->bat_desc.type			= POWER_SUPPLY_TYPE_BATTERY;
	di->bat_desc.properties		= ds2784_props;
	di->bat_desc.num_properties	= ARRAY_SIZE(ds2784_props);
	di->bat_desc.get_property	= ds2784_get_property;

	psy_cfg.drv_data = di;
	if (dev->of_node)
		psy_cfg.of_node = dev->of_node;

	di->inited = true;
	ds2784_battery_read_status(di);

	di->bat = power_supply_register(dev, &di->bat_desc, &psy_cfg);
	if (IS_ERR(di->bat)) {
		dev_err(di->dev, "failed to register battery\n");
		retval = PTR_ERR(di->bat);
		goto out;
	}

out:
	return retval;
}

static int ds2784_battery_remove(struct platform_device *pdev)
{
	struct ds2784_device_info *di = platform_get_drvdata(pdev);

	power_supply_unregister(di->bat);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id ds2784_of_match[] = {
	{ .compatible = "maxim,ds2784" },
	{}
};
MODULE_DEVICE_TABLE(of, ds2784_of_match);
#endif

static struct platform_driver ds2784_battery_driver = {
	.driver = {
		.name = "ds2784-battery",
	},
	.probe	= ds2784_battery_probe,
	.remove	= ds2784_battery_remove,
};
module_platform_driver(ds2784_battery_driver);


MODULE_AUTHOR("Samsung");
MODULE_DESCRIPTION("Maxim/Dallas DS2784 Stand-Alone Fuel Gauge IC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ds2784-battery");
