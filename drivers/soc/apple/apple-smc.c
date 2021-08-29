// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <linux/apple-rtkit.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/types.h>

struct apple_smc {
	struct device *dev;
	struct apple_rtkit *rtk;

	struct clk_bulk_data *clks;
	int num_clks;

	struct resource shmem_res;
	void __iomem *shmem;

	struct mutex cmd_lock;
	struct completion cmd_completion;
	void __iomem *cmd_arg_buffer;
	u8 cmd_tag;
	u64 cmd_reply;
	int cmd_status;
	bool cmd_expect_reply;
	bool cmd_use_tags;

	struct irq_domain *irq_domain;
};

#define APPLE_SMC_BOOT_TIMEOUT msecs_to_jiffies(1000)

#define FOURCC(a, b, c, d)                                                     \
	(((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | ((u32)(d)))

#define APPLE_SMC_KEY_NOTIFICATION_EN FOURCC('N', 'T', 'A', 'P')

#define APPLE_SMC_ENDPOINT 0x20

#define APPLE_SMC_ARGBFR_SIZE 0x4000

#define APPLE_SMC_CMD_ARG0 GENMASK(63, 32)
#define APPLE_SMC_CMD_ARG1 GENMASK(31, 16)
#define APPLE_SMC_CMD_TAG GENMASK(15, 12)
#define APPLE_SMC_CMD_OPCODE GENMASK(7, 0)

#define APPLE_SMC_OPCODE_READ_KEY 0x10
#define APPLE_SMC_OPCODE_WRITE_KEY 0x11
#define APPLE_SMC_OPCODE_GET_KEY_BY_INDEX 0x12
#define APPLE_SMC_OPCODE_GET_KEY_INFO 0x13
#define APPLE_SMC_OPCODE_GET_ADDR 0x17
#define APPLE_SMC_OPCODE_READ_KEY_PAYLOAD 0x20

#define APPLE_SMC_REPLY_TYPE GENMASK(7, 0)
#define APPLE_SMC_REPLY_NOTIFICATION 0x18

#define APPLE_SMC_NOTIFICATION_TYPE GENMASK(63, 56)
#define APPLE_SMC_NOTIFICATION_PAYLOAD GENMASK(55, 32)

#define APPLE_SMC_NOTIFICATION_SYSTEM_STATE 0x70

static int apple_smc_command(struct apple_smc *smc, u8 opcode, u32 arg0,
			     u16 arg1, const void *bfr_in, size_t size_in,
			     void *bfr_out, size_t size_out, u64 *reply)
{
	int ret;
	u64 cmd;

	if (size_in > APPLE_SMC_ARGBFR_SIZE)
		return -EINVAL;
	if (size_out > APPLE_SMC_ARGBFR_SIZE)
		return -EINVAL;
	if (size_in && !bfr_in)
		return -EINVAL;
	if (size_out && !bfr_out)
		return -EINVAL;

	mutex_lock(&smc->cmd_lock);
	reinit_completion(&smc->cmd_completion);
	smc->cmd_expect_reply = true;

	if (size_in)
		memcpy_toio(smc->cmd_arg_buffer, bfr_in, size_in);

	cmd = FIELD_PREP(APPLE_SMC_CMD_OPCODE, opcode);

	if (smc->cmd_use_tags)
		cmd |= FIELD_PREP(APPLE_SMC_CMD_TAG, smc->cmd_tag);

	cmd |= FIELD_PREP(APPLE_SMC_CMD_ARG0, arg0);
	cmd |= FIELD_PREP(APPLE_SMC_CMD_ARG1, arg1);

	dev_dbg(smc->dev, "TX %016llx\n", cmd);

	ret = apple_rtkit_send_message(smc->rtk, APPLE_SMC_ENDPOINT, cmd);
	if (ret)
		goto out;

	wait_for_completion(&smc->cmd_completion);

	if (smc->cmd_use_tags)
		smc->cmd_tag = (smc->cmd_tag + 1) & 0xf;

	ret = smc->cmd_status;
	if (ret)
		goto out;

	if (reply)
		*reply = smc->cmd_reply;
	if (size_out)
		memcpy_fromio(bfr_out, smc->cmd_arg_buffer, size_out);

out:
	mutex_unlock(&smc->cmd_lock);
	return ret;
}

static int apple_smc_write_key(struct apple_smc *smc, u32 key, void *data,
			       size_t len)
{
	return apple_smc_command(smc, APPLE_SMC_OPCODE_WRITE_KEY, key, len,
				 data, len, NULL, 0, NULL);
}

static int apple_smc_write_key_u8(struct apple_smc *smc, u32 key, u8 value)
{
	return apple_smc_write_key(smc, key, &value, sizeof(u8));
}

static void apple_smc_handle_notification(struct apple_smc *smc, u64 message)
{
	unsigned long flags;
	u8 type = FIELD_GET(APPLE_SMC_NOTIFICATION_TYPE, message);
	u32 payload = FIELD_GET(APPLE_SMC_NOTIFICATION_PAYLOAD, message);
	dev_warn(smc->dev, "notification: %016llx (type: %02x, payload: %08x)",
		 message, type, payload);

	// FIXME
	if (type != 0x70)
		return;
	if (payload != 0x200000)
		return;

	local_irq_save(flags);
	generic_handle_domain_irq(smc->irq_domain, 21);
	local_irq_restore(flags);
}

static void apple_smc_recv_message(void *cookie, u8 endpoint, u64 message)
{
	struct apple_smc *smc = cookie;
	u8 type;

	dev_dbg(smc->dev, "RX: %016llx on EP %02x\n", message, endpoint);

	if (endpoint != APPLE_SMC_ENDPOINT) {
		dev_warn(smc->dev, "RX: %016llx on unknown EP %02x\n", message,
			 endpoint);
		return;
	}

	type = FIELD_GET(APPLE_SMC_REPLY_TYPE, message);

	if (type == APPLE_SMC_REPLY_NOTIFICATION) {
		apple_smc_handle_notification(smc, message);
		return;
	}

	if (!smc->cmd_expect_reply) {
		dev_warn(smc->dev, "unexpected message %016llx", message);
		return;
	}

	if (smc->cmd_use_tags) {
		u8 tag = FIELD_GET(APPLE_SMC_CMD_TAG, message);
		if (tag != smc->cmd_tag) {
			dev_warn(smc->dev,
				 "tag mismatch, got %02x but expected %02x\n",
				 tag, smc->cmd_tag);
			smc->cmd_status = -EINVAL;
			complete(&smc->cmd_completion);
			return;
		}
	}

	smc->cmd_status = 0;
	smc->cmd_reply = message;
	smc->cmd_expect_reply = false;
	complete(&smc->cmd_completion);
}

static __iomem void *apple_smc_shmem_map(void *cookie, dma_addr_t addr,
					 size_t len)
{
	struct apple_smc *smc = cookie;
	size_t offset = addr - smc->shmem_res.start;

	if (addr < smc->shmem_res.start)
		return NULL;
	if (offset >= resource_size(&smc->shmem_res))
		return NULL;
	if ((offset + len) > resource_size(&smc->shmem_res))
		return NULL;

	return smc->shmem + offset;
}

static void apple_smc_unmap(void *cookie, void __iomem *ptr, dma_addr_t addr,
			    size_t len)
{
}

static inline int apple_smc_irqd_xlate(struct irq_domain *d,
				       struct device_node *node,
				       const u32 *intspec, unsigned int intsize,
				       unsigned long *out_hwirq,
				       unsigned int *out_type)
{
	// FIXME
	if (intspec[0] != 21)
		return -EINVAL;

	*out_hwirq = intspec[0];
	*out_type = IRQ_TYPE_EDGE_RISING; // TODO: figure this out correctly
	return 0;
}

static struct irq_chip apple_smc_irq_chip = {
	.name = "Apple SMC Notification Interrupt Chip",
};

static int apple_smc_irqd_map(struct irq_domain *d, unsigned int virq,
			      irq_hw_number_t hw)
{
	irq_domain_set_info(d, virq, hw, &apple_smc_irq_chip, d->host_data,
			    handle_simple_irq, NULL, NULL);
	return 0;
}

static const struct irq_domain_ops apple_smc_irq_domain_ops = {
	.xlate = apple_smc_irqd_xlate,
	.map = apple_smc_irqd_map,
};

static const struct apple_rtkit_ops apple_smc_rtkit_ops = {
	.flags = APPLE_RTKIT_SHMEM_OWNER_RTKIT | APPLE_RTKIT_RECV_ATOMIC,
	.recv_message = apple_smc_recv_message,
	.shmem_map = apple_smc_shmem_map,
	.shmem_unmap = apple_smc_unmap,
};

static int apple_smc_probe(struct platform_device *pdev)
{
	int ret;
	u64 shmem_arg_addr;
	struct apple_smc *smc;
	struct resource *coproc_res;
	struct device_node *shmem;
	struct device *dev = &pdev->dev;

	smc = devm_kzalloc(dev, sizeof(*smc), GFP_KERNEL);
	if (!smc)
		return -ENOMEM;
	platform_set_drvdata(pdev, smc);

	smc->dev = dev;
	init_completion(&smc->cmd_completion);
	mutex_init(&smc->cmd_lock);

	shmem = of_parse_phandle(dev->of_node, "shmem", 0);
	ret = of_address_to_resource(shmem, 0, &smc->shmem_res);
	of_node_put(shmem);
	if (ret) {
		dev_err(dev, "failed to get shared memory\n");
		return ret;
	}

	coproc_res =
		platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR(coproc_res))
		return PTR_ERR(coproc_res);

	smc->shmem = devm_ioremap_resource(smc->dev, &smc->shmem_res);
	if (IS_ERR(smc->shmem))
		return PTR_ERR(smc->shmem);

	ret = devm_clk_bulk_get_all(dev, &smc->clks);
	if (ret < 0)
		return ret;
	smc->num_clks = ret;

	ret = clk_bulk_prepare_enable(smc->num_clks, smc->clks);
	if (ret)
		return ret;

	smc->rtk = apple_rtkit_init(dev, smc, coproc_res, NULL, 0,
				    &apple_smc_rtkit_ops);
	if (IS_ERR(smc->rtk))
		return PTR_ERR(smc->rtk);

	ret = apple_rtkit_boot_wait(smc->rtk, APPLE_SMC_BOOT_TIMEOUT);
	if (ret)
		return ret;

	ret = apple_rtkit_start_ep(smc->rtk, APPLE_SMC_ENDPOINT);
	if (ret)
		return ret;

	ret = apple_smc_command(smc, APPLE_SMC_OPCODE_GET_ADDR, 0, 0, NULL, 0,
				NULL, 0, &shmem_arg_addr);
	if (ret)
		return ret;

	smc->cmd_use_tags = true;
	dev_info(smc->dev, "command argument buffer at %016llx\n",
		 shmem_arg_addr);

	smc->cmd_arg_buffer =
		apple_smc_shmem_map(smc, shmem_arg_addr, APPLE_SMC_ARGBFR_SIZE);

	ret = apple_smc_write_key_u8(smc, APPLE_SMC_KEY_NOTIFICATION_EN, 1);
	if (ret)
		return ret;

	smc->irq_domain = irq_domain_add_tree(
		smc->dev->of_node, &apple_smc_irq_domain_ops, smc);
	if (!smc->irq_domain)
		return -EINVAL;

	return 0;
}

static const struct of_device_id apple_smc_of_match[] = {
	{
		.compatible = "apple,t8103-smc",
	},
	{}
};
MODULE_DEVICE_TABLE(of, apple_smc_of_match);

static struct platform_driver apple_smc_driver = {
	.driver = {
		.name = "apple-smc",
		.of_match_table = apple_smc_of_match,
	},
	.probe = apple_smc_probe,
};
module_platform_driver(apple_smc_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_DESCRIPTION("Apple SMC driver");
