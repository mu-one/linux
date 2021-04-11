// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple SoC clock/power gating driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/clk-provider.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/module.h>

#define APPLE_CLOCK_TARGET_MODE GENMASK(3, 0)
#define APPLE_CLOCK_ACTUAL_MODE GENMASK(7, 4)

#define APPLE_CLOCK_ENABLE 0xf
#define APPLE_CLOCK_DISABLE 0x0

#define APPLE_CLOCK_ENDISABLE_TIMEOUT 100

struct apple_clk_gate {
	struct clk_hw hw;
	void __iomem *reg;
};

#define to_apple_clk_gate(_hw) container_of(_hw, struct apple_clk_gate, hw)

static int apple_clk_gate_endisable(struct clk_hw *hw, int enable)
{
	struct apple_clk_gate *gate = to_apple_clk_gate(hw);
	u32 reg;
	u32 mode;

	if (enable)
		mode = APPLE_CLOCK_ENABLE;
	else
		mode = APPLE_CLOCK_DISABLE;

	reg = readl(gate->reg);
	reg &= ~APPLE_CLOCK_TARGET_MODE;
	reg |= FIELD_PREP(APPLE_CLOCK_TARGET_MODE, mode);
	writel(reg, gate->reg);

	return readl_poll_timeout_atomic(
		gate->reg, reg,
		(FIELD_GET(APPLE_CLOCK_ACTUAL_MODE, reg) == mode), 1,
		APPLE_CLOCK_ENDISABLE_TIMEOUT);
}

static int apple_clk_gate_enable(struct clk_hw *hw)
{
	return apple_clk_gate_endisable(hw, 1);
}

static void apple_clk_gate_disable(struct clk_hw *hw)
{
	apple_clk_gate_endisable(hw, 0);
}

static int apple_clk_gate_is_enabled(struct clk_hw *hw)
{
	struct apple_clk_gate *gate = to_apple_clk_gate(hw);
	u32 reg;

	reg = readl(gate->reg);
	return FIELD_GET(APPLE_CLOCK_ACTUAL_MODE, reg) == APPLE_CLOCK_ENABLE;
}

static const struct clk_ops apple_clk_gate_ops = {
	.enable = apple_clk_gate_enable,
	.disable = apple_clk_gate_disable,
	.is_enabled = apple_clk_gate_is_enabled,
};

static struct clk_hw *
apple_clk_gate_register(struct device *dev, const char *name, void __iomem *reg,
			struct clk_parent_data *parent_data)
{
	struct apple_clk_gate *gate;
	struct clk_hw *hw;
	struct clk_init_data init;
	int ret;

	memset(&init, 0, sizeof(init));
	gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);

	gate->reg = reg;
	hw = &gate->hw;
	hw->init = &init;

	init.name = name;
	init.ops = &apple_clk_gate_ops;
	init.flags = 0;
	init.parent_names = NULL;
	init.parent_data = parent_data;
	init.num_parents = 1;

	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		return ERR_PTR(ret);

	return hw;
}

struct clk_hw *apple_clk_hw_onecell_get(struct of_phandle_args *clkspec,
					void *data)
{
	struct clk_hw *hw = NULL;
	struct clk_hw_onecell_data *hw_data = data;
	unsigned int idx = clkspec->args[0];

	if (idx % 8) {
		pr_err("%s: unaligned index: %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	idx /= 8;
	if (idx >= hw_data->num) {
		pr_err("%s: index out of bounds: %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	hw = hw_data->hws[idx];
	if (!hw)
		return ERR_PTR(-EINVAL);
	return hw;
}

static int apple_gate_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct clk_hw_onecell_data *data;
	struct resource *res;
	struct property *prop;
	void __iomem *regs;
	const __be32 *p;
	int i, index;
	int num_clocks, max_clocks;
	int ret;
	const char *clk_name;
	struct clk_parent_data parent_data[] = {
		{ .index = 0 },
	};

	num_clocks = of_property_count_u32_elems(node, "clock-indices");
	if (num_clocks < 1)
		return -EINVAL;

	if (of_property_read_u32_index(node, "clock-indices", num_clocks - 1,
				       &max_clocks))
		return -EINVAL;
	if (WARN_ON(max_clocks % 8))
		return -EINVAL;
	max_clocks /= 8;
	max_clocks++;

	data = devm_kzalloc(dev, struct_size(data, hws, max_clocks),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = max_clocks;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR(res))
		return PTR_ERR(res);
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	ret = devm_of_clk_add_hw_provider(dev, apple_clk_hw_onecell_get, data);
	if (ret < 0)
		return ret;

	i = 0;
	of_property_for_each_u32 (node, "clock-indices", prop, p, index) {
		ret = of_property_read_string_index(node, "clock-output-names",
						    i, &clk_name);
		if (ret)
			return ret;
		if (index % 8)
			return -EINVAL;

		parent_data[0].index = i;

		data->hws[index / 8] = apple_clk_gate_register(
			dev, clk_name, regs + index, parent_data);
		if (IS_ERR(data->hws[index / 8]))
			return PTR_ERR(data->hws[index / 8]);

		i += 1;
	}

	return 0;
}

static const struct of_device_id apple_gate_clk_of_match[] = {
	{ .compatible = "apple,t8103-gate-clock-controller" },
	{ .compatible = "apple,gate-clock-controller" },
	{}
};

MODULE_DEVICE_TABLE(of, apple_gate_clk_of_match);

static struct platform_driver apple_gate_clkdriver = {
	.probe = apple_gate_clk_probe,
	.driver = {
		.name = "apple-gate-clock-controller",
		.of_match_table = apple_gate_clk_of_match,
	},
};

MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_DESCRIPTION("Clock gating driver for Apple SoCs");
MODULE_LICENSE("GPL v2");

module_platform_driver(apple_gate_clkdriver);
