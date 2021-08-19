// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple mailbox driver
 *
 * Copyright (C) 2021 The Asahi Linux Contributors
 *
 * This mailbox hardware consists of two FIFOs used to exchange 64+32 bit
 * messages between the main CPU and a co-processor. Multiple instances
 * of this mailbox can be found on Apple SoCs.
 * Various clients implement different IPC protocols based on these simple
 * messages and shared memory buffers.
 *
 * Both the main CPU and the co-processor see the same set of registers but
 * the first FIFO (A2I) is always used to transfer messages from the application
 * processor (us) to the I/O processor and the second one (I2A) for the
 * other direction.
 */

#include <linux/apple-mailbox.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#define APPLE_ASC_MBOX_A2I_CONTROL 0x110
#define APPLE_ASC_MBOX_I2A_CONTROL 0x114
#define APPLE_ASC_MBOX_CONTROL_FULL BIT(16)
#define APPLE_ASC_MBOX_CONTROL_EMPTY BIT(17)

#define APPLE_ASC_MBOX_A2I_SEND0 0x800
#define APPLE_ASC_MBOX_A2I_SEND1 0x808
#define APPLE_ASC_MBOX_A2I_RECV0 0x810
#define APPLE_ASC_MBOX_A2I_RECV1 0x818

#define APPLE_ASC_MBOX_I2A_SEND0 0x820
#define APPLE_ASC_MBOX_I2A_SEND1 0x828
#define APPLE_ASC_MBOX_I2A_RECV0 0x830
#define APPLE_ASC_MBOX_I2A_RECV1 0x838

#define APPLE_M3_MBOX_A2I_CONTROL 0x50
#define APPLE_M3_MBOX_A2I_SEND0 0x60
#define APPLE_M3_MBOX_A2I_SEND1 0x68
#define APPLE_M3_MBOX_A2I_RECV0 0x70
#define APPLE_M3_MBOX_A2I_RECV1 0x78

#define APPLE_M3_MBOX_I2A_CONTROL 0x80
#define APPLE_M3_MBOX_I2A_SEND0 0x90
#define APPLE_M3_MBOX_I2A_SEND1 0x98
#define APPLE_M3_MBOX_I2A_RECV0 0xa0
#define APPLE_M3_MBOX_I2A_RECV1 0xa8

#define APPLE_M3_MBOX_CONTROL_FULL BIT(16)
#define APPLE_M3_MBOX_CONTROL_EMPTY BIT(17)

#define APPLE_M3_MBOX_IRQ_ENABLE 0x48
#define APPLE_M3_MBOX_IRQ_A2I_EMPTY BIT(0)
#define APPLE_M3_MBOX_IRQ_A2I_NOT_EMPTY BIT(1)
#define APPLE_M3_MBOX_IRQ_I2A_EMPTY BIT(2)
#define APPLE_M3_MBOX_IRQ_I2A_NOT_EMPTY BIT(3)

#define APPLE_MBOX_MSG1_OUTCNT GENMASK(56, 52)
#define APPLE_MBOX_MSG1_INCNT GENMASK(51, 48)
#define APPLE_MBOX_MSG1_OUTPTR GENMASK(47, 44)
#define APPLE_MBOX_MSG1_INPTR GENMASK(43, 40)
#define APPLE_MBOX_MSG1_MSG GENMASK(31, 0)

struct apple_mbox_hw_regs {
	unsigned int control_full;
	unsigned int control_empty;

	void __iomem *a2i_control;
	void __iomem *a2i_send0;
	void __iomem *a2i_send1;

	void __iomem *i2a_control;
	void __iomem *i2a_recv0;
	void __iomem *i2a_recv1;
};

struct apple_mbox {
	struct apple_mbox_hw_regs regs;
	int irq_recv_not_empty;
	int irq_send_empty;

	struct clk_bulk_data *clks;
	int num_clks;

	struct mbox_chan chan;

	struct device *dev;
	struct mbox_controller controller;
};

static bool apple_mbox_hw_can_send(struct apple_mbox *apple_mbox)
{
	u32 mbox_ctrl = readl_relaxed(apple_mbox->regs.a2i_control);

	return !(mbox_ctrl & apple_mbox->regs.control_full);
}

static void apple_mbox_hw_send(struct apple_mbox *apple_mbox,
			       struct apple_mbox_msg *msg)
{
	WARN_ON(!apple_mbox_hw_can_send(apple_mbox));

	dev_dbg(apple_mbox->dev, "> TX %016llx %08x\n", msg->msg0, msg->msg1);

	/*
	 * this message may be related to a shared memory buffer and we must
	 * ensure all previous writes to normal memory are visible before
	 * submitting it.
	 */
	dma_wmb();

	writeq_relaxed(msg->msg0, apple_mbox->regs.a2i_send0);
	writeq_relaxed(FIELD_PREP(APPLE_MBOX_MSG1_MSG, msg->msg1),
		       apple_mbox->regs.a2i_send1);
}

static bool apple_mbox_hw_can_recv(struct apple_mbox *apple_mbox)
{
	u32 mbox_ctrl = readl_relaxed(apple_mbox->regs.i2a_control);

	return !(mbox_ctrl & apple_mbox->regs.control_empty);
}

static void apple_mbox_hw_recv(struct apple_mbox *apple_mbox,
			       struct apple_mbox_msg *msg)
{
	WARN_ON(!apple_mbox_hw_can_recv(apple_mbox));

	msg->msg0 = readq_relaxed(apple_mbox->regs.i2a_recv0);
	msg->msg1 = FIELD_GET(APPLE_MBOX_MSG1_MSG,
			      readq_relaxed(apple_mbox->regs.i2a_recv1));

	dev_dbg(apple_mbox->dev, "< RX %016llx %08x\n", msg->msg0, msg->msg1);

	/*
	 * this message may be related to a shared memory buffer and we must
	 * ensure any following reads from normal memory only happen after
	 * reading this meesage.
	 */
	dma_rmb();
}

static int apple_mbox_chan_send_data(struct mbox_chan *chan, void *data)
{
	struct apple_mbox *apple_mbox = chan->con_priv;
	struct apple_mbox_msg *msg = data;

	if (!apple_mbox_hw_can_send(apple_mbox)) {
		dev_dbg(apple_mbox->dev, "FIFO full, waiting for IRQ\n");
		return -EBUSY;
	}
	apple_mbox_hw_send(apple_mbox, msg);
	return 0;
}

static irqreturn_t apple_mbox_send_empty_irq(int irq, void *data)
{
	struct apple_mbox *apple_mbox = data;

	dev_dbg(apple_mbox->dev, "got FIFO empty IRQ\n");
	disable_irq_nosync(apple_mbox->irq_send_empty);
	mbox_chan_txready(&apple_mbox->chan);
	return IRQ_HANDLED;
}

static irqreturn_t apple_mbox_recv_irq(int irq, void *data)
{
	struct apple_mbox *apple_mbox = data;
	struct apple_mbox_msg msg;

	while (apple_mbox_hw_can_recv(apple_mbox)) {
		apple_mbox_hw_recv(apple_mbox, &msg);
		mbox_chan_received_data(&apple_mbox->chan, (void *)&msg);
	}

	return IRQ_HANDLED;
}

static struct mbox_chan *apple_mbox_of_xlate(struct mbox_controller *mbox,
					     const struct of_phandle_args *spec)
{
	struct apple_mbox *apple_mbox = dev_get_drvdata(mbox->dev);

	if (spec->args_count != 0)
		return ERR_PTR(-EINVAL);
	if (apple_mbox->chan.con_priv)
		return ERR_PTR(-EBUSY);

	apple_mbox->chan.con_priv = apple_mbox;
	return &apple_mbox->chan;
}

static int apple_mbox_chan_startup(struct mbox_chan *chan)
{
	struct apple_mbox *apple_mbox = chan->con_priv;
	enable_irq(apple_mbox->irq_recv_not_empty);
	return 0;
}

static void apple_mbox_chan_shutdown(struct mbox_chan *chan)
{
	struct apple_mbox *apple_mbox = chan->con_priv;
	disable_irq(apple_mbox->irq_recv_not_empty);
}

static void apple_mbox_chan_request_irq(struct mbox_chan *chan)
{
	struct apple_mbox *apple_mbox = chan->con_priv;
	enable_irq(apple_mbox->irq_send_empty);
}

static const struct mbox_chan_ops apple_mbox_ops = {
	.send_data = apple_mbox_chan_send_data,
	.request_irq = apple_mbox_chan_request_irq,
	.startup = apple_mbox_chan_startup,
	.shutdown = apple_mbox_chan_shutdown,
};

enum { APPLE_MBOX_ASC, APPLE_MBOX_M3 };

static const struct of_device_id apple_mbox_of_match[] = {
	{ .compatible = "apple,t8103-asc-mailbox",
	  .data = (void *)APPLE_MBOX_ASC },
	{ .compatible = "apple,t8103-m3-mailbox",
	  .data = (void *)APPLE_MBOX_M3 },
	{}
};
MODULE_DEVICE_TABLE(of, apple_mbox_of_match);

static int apple_mbox_probe(struct platform_device *pdev)
{
	int ret;
	const struct of_device_id *match;
	void __iomem *regs;
	struct apple_mbox *mbox;
	struct device *dev = &pdev->dev;

	match = of_match_node(apple_mbox_of_match, pdev->dev.of_node);
	if (!match)
		return -EINVAL;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	platform_set_drvdata(pdev, mbox);

	mbox->dev = dev;
	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	switch ((uintptr_t)match->data) {
	case APPLE_MBOX_ASC:
		mbox->regs.control_full = APPLE_ASC_MBOX_CONTROL_FULL;
		mbox->regs.control_empty = APPLE_ASC_MBOX_CONTROL_EMPTY;
		mbox->regs.a2i_control = regs + APPLE_ASC_MBOX_A2I_CONTROL;
		mbox->regs.a2i_send0 = regs + APPLE_ASC_MBOX_A2I_SEND0;
		mbox->regs.a2i_send1 = regs + APPLE_ASC_MBOX_A2I_SEND1;
		mbox->regs.i2a_control = regs + APPLE_ASC_MBOX_I2A_CONTROL;
		mbox->regs.i2a_recv0 = regs + APPLE_ASC_MBOX_I2A_RECV0;
		mbox->regs.i2a_recv1 = regs + APPLE_ASC_MBOX_I2A_RECV1;
		break;
	case APPLE_MBOX_M3:
		mbox->regs.control_full = APPLE_M3_MBOX_CONTROL_FULL;
		mbox->regs.control_empty = APPLE_M3_MBOX_CONTROL_EMPTY;
		mbox->regs.a2i_control = regs + APPLE_M3_MBOX_A2I_CONTROL;
		mbox->regs.a2i_send0 = regs + APPLE_M3_MBOX_A2I_SEND0;
		mbox->regs.a2i_send1 = regs + APPLE_M3_MBOX_A2I_SEND1;
		mbox->regs.i2a_control = regs + APPLE_M3_MBOX_I2A_CONTROL;
		mbox->regs.i2a_recv0 = regs + APPLE_M3_MBOX_I2A_RECV0;
		mbox->regs.i2a_recv1 = regs + APPLE_M3_MBOX_I2A_RECV1;

		writel_relaxed(APPLE_M3_MBOX_IRQ_A2I_EMPTY |
				       APPLE_M3_MBOX_IRQ_I2A_NOT_EMPTY,
			       regs + APPLE_M3_MBOX_IRQ_ENABLE);
		break;
	default:
		return -EINVAL;
	}

	mbox->irq_recv_not_empty =
		platform_get_irq_byname(pdev, "recv-not-empty");
	if (mbox->irq_recv_not_empty < 0)
		return -ENODEV;

	mbox->irq_send_empty = platform_get_irq_byname(pdev, "send-empty");
	if (mbox->irq_send_empty < 0)
		return -ENODEV;

	ret = devm_clk_bulk_get_all(dev, &mbox->clks);
	if (ret < 0)
		return ret;
	mbox->num_clks = ret;

	ret = clk_bulk_prepare_enable(mbox->num_clks, mbox->clks);
	if (ret)
		return ret;

	mbox->controller.dev = mbox->dev;
	mbox->controller.num_chans = 1;
	mbox->controller.chans = &mbox->chan;
	mbox->controller.ops = &apple_mbox_ops;
	mbox->controller.of_xlate = &apple_mbox_of_xlate;
	mbox->controller.txdone_fifo = true;

	ret = devm_request_irq(dev, mbox->irq_recv_not_empty,
			       apple_mbox_recv_irq, IRQF_NO_AUTOEN,
			       dev_name(dev), mbox);
	if (ret)
		goto err_clk_disable;
	ret = devm_request_irq(dev, mbox->irq_send_empty,
			       apple_mbox_send_empty_irq, IRQF_NO_AUTOEN,
			       dev_name(dev), mbox);
	if (ret)
		goto err_clk_disable;

	ret = devm_mbox_controller_register(dev, &mbox->controller);
	if (ret)
		goto err_clk_disable;
	return ret;

err_clk_disable:
	clk_bulk_disable_unprepare(mbox->num_clks, mbox->clks);
	return ret;
}

static struct platform_driver apple_mbox_driver = {
	.driver = {
		.name = "apple-mailbox",
		.of_match_table = apple_mbox_of_match,
	},
	.probe = apple_mbox_probe,
};
module_platform_driver(apple_mbox_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_DESCRIPTION("Apple Mailbox driver");
