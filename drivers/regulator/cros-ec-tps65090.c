// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Google, Inc
 *
 * Expose the ChromeOS EC regulator information.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>

#define MAX_REGULATORS		10

struct cros_ec_tps65090_regulator {
	struct regulator_desc	desc;
	struct regulator_dev	*rdev;
	struct cros_ec_device	*ec;

	u32			control_reg;
};

struct cros_ec_tps65090_data {
	struct cros_ec_tps65090_regulator *regulators[MAX_REGULATORS];
};

/* Control register for FETs(7) range from 15 -> 21 and correspond to
   FET number (1-7) on the EC side */
#define reg_to_cros_ec_fet_index(reg) (reg->control_reg - 14)

static int cros_ec_tps65090_fet_enable(struct regulator_dev *dev)
{
	struct cros_ec_tps65090_regulator *reg = rdev_get_drvdata(dev);
	struct ec_params_ldo_set cmd = {
		.index = reg_to_cros_ec_fet_index(reg),
		.state = EC_LDO_STATE_ON,
	};

	return cros_ec_cmd(reg->ec, 0, EC_CMD_LDO_SET, &cmd,
			   sizeof(cmd), NULL, 0);
}

static int cros_ec_tps65090_fet_disable(struct regulator_dev *dev)
{
	struct cros_ec_tps65090_regulator *reg = rdev_get_drvdata(dev);
	struct ec_params_ldo_set cmd = {
		.index = reg_to_cros_ec_fet_index(reg),
		.state = EC_LDO_STATE_OFF,
	};

	return cros_ec_cmd(reg->ec, 0, EC_CMD_LDO_SET, &cmd,
			   sizeof(cmd), NULL, 0);
}

static int cros_ec_tps65090_fet_is_enabled(struct regulator_dev *dev)
{
	struct cros_ec_tps65090_regulator *reg = rdev_get_drvdata(dev);
	struct ec_params_ldo_get cmd = {
		.index = reg_to_cros_ec_fet_index(reg),
	};
	struct ec_response_ldo_get resp;
	int ret;

	ret = cros_ec_cmd(reg->ec, 0, EC_CMD_LDO_GET, &cmd,
			  sizeof(cmd), &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	return resp.state;
}

static struct regulator_ops cros_ec_tps65090_fet_ops = {
	.enable	= cros_ec_tps65090_fet_enable,
	.disable = cros_ec_tps65090_fet_disable,
	.is_enabled = cros_ec_tps65090_fet_is_enabled,
	.set_suspend_enable = cros_ec_tps65090_fet_enable,
	.set_suspend_disable = cros_ec_tps65090_fet_disable,
};

static int cros_ec_tps65090_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_device *ec = dev_get_drvdata(dev->parent);
	struct device_node *reg_np, *np;
	struct cros_ec_tps65090_data *data;
	struct cros_ec_tps65090_regulator *reg;
	struct regulator_init_data *init_data;
	struct regulator_config cfg = {};
	u32 id = 0;

	if (!ec)
		return dev_err_probe(dev, -EINVAL, "no EC device found\n");

	if (!dev->of_node)
		return dev_err_probe(dev, -EINVAL, "no device tree data available\n");

	data = devm_kzalloc(dev, sizeof(struct cros_ec_tps65090_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	reg_np = of_find_node_by_name(dev->of_node, "regulators");
	if (!reg_np)
		return dev_err_probe(dev, -EINVAL,
				     "no OF regulator data found at %s\n",
				     dev->of_node->full_name);

	for_each_child_of_node(reg_np, np) {
		init_data = of_get_regulator_init_data(dev, np, &reg->desc);
		if (!init_data) {
			dev_err(dev, "regulator_init_data failed for %s\n",
				np->full_name);
			goto err;
		}

                if (init_data->constraints.min_uV != init_data->constraints.max_uV) {
                        dev_err(dev,
                                 "cros-ec-tps65090 regulator specified with variable voltages\n");
                        goto err;
                }

		reg = devm_kzalloc(dev, sizeof(struct cros_ec_tps65090_regulator),
				   GFP_KERNEL);
		reg->desc.name = kstrdup(of_get_property(np, "regulator-name",
							 NULL), GFP_KERNEL);
		reg->ec = ec;
		if (!reg->desc.name) {
			dev_err(dev, "no regulator-name specified at %s\n",
				np->full_name);
			goto err;
		}

		if (of_property_read_u32(np, "ti,tps65090-control-reg",
					 &reg->control_reg)) {
			dev_err(dev, "no control-reg property at %s\n",
				np->full_name);
			goto err;
		}

		reg->desc.id = id;
		reg->desc.ops = &cros_ec_tps65090_fet_ops;
		reg->desc.type = REGULATOR_VOLTAGE;
		reg->desc.owner = THIS_MODULE;
		reg->desc.n_voltages = 1;
		reg->desc.fixed_uV = init_data->constraints.min_uV;

		cfg.dev = dev->parent;
		cfg.driver_data = reg;
		cfg.of_node = np;
		cfg.init_data = init_data;

		reg->rdev = devm_regulator_register(dev, &reg->desc, &cfg);
		dev_dbg(dev, "%s supply registered (FET%d)\n", reg->desc.name,
			reg_to_cros_ec_fet_index(reg));

		data->regulators[id++] = reg;
	}

	platform_set_drvdata(pdev, data);
	of_node_put(reg_np);

	return 0;

err:
	dev_err(dev, "bad OF regulator data in %s\n", reg_np->full_name);
	of_node_put(reg_np);
	return -EINVAL;
}

#if 0

This works but needs verification of rstate->disabled
(which is now named enabled) behavior

#ifdef CONFIG_PM_SLEEP
static int cros_ec_tps65090_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct cros_ec_tps65090_data *data = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < MAX_REGULATORS; i++)
		if (data->regulators[i]) {
			struct regulator_dev *rdev = data->regulators[i]->rdev;
			struct regulator_state *rstate =
				&rdev->constraints->state_mem;
			struct regulator_ops *ops = rdev->desc->ops;
			if (rstate->disabled)
				ops->set_suspend_disable(rdev);
		}

	return 0;
}

static int cros_ec_tps65090_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct cros_ec_tps65090_data *data = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < MAX_REGULATORS; i++)
		if (data->regulators[i]) {
			struct regulator_dev *rdev = data->regulators[i]->rdev;
			struct regulator_state *rstate =
				&rdev->constraints->state_mem;
			struct regulator_ops *ops = rdev->desc->ops;
			if (rstate->disabled && (rdev->use_count > 0 ||
						 rdev->constraints->always_on))
				ops->enable(rdev);
		}

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(cros_ec_tps65090_pm_ops, cros_ec_tps65090_suspend,
			 cros_ec_tps65090_resume);
#endif

static const struct of_device_id cros_ec_tps65090_of_match[] = {
	{ .compatible = "ti,cros-ec-tps65090", },
	{}
};
MODULE_DEVICE_TABLE(of, cros_ec_tps65090_of_match);

static struct platform_driver cros_ec_tps65090_driver = {
	.probe = cros_ec_tps65090_probe,
	.driver = {
		.name = "cros-ec-tps65090",
		.of_match_table = cros_ec_tps65090_of_match,
//		.pm = &cros_ec_tps65090_pm_ops,
	},
};

module_platform_driver(cros_ec_tps65090_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ChromeOS EC controlled TPS65090 FET regulators");
