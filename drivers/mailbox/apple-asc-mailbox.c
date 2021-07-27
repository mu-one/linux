// SPDX-License-Identifier: GPL-2.0-only

#include "apple-asc-mailbox.h"

#define CREATE_TRACE_POINTS
#include "apple-asc-mailbox-trace.h"

static bool apple_mbox_hw_can_send(struct apple_mbox *apple_mbox)
{
	u32 mbox_ctrl = readl(apple_mbox->regs + APPLE_IOP_A2I_CONTROL);
	return !(mbox_ctrl & APPLE_IOP_A2I_CONTROL_FULL);
}

static void apple_mbox_hw_send(struct apple_mbox *apple_mbox,
			       struct apple_mbox_msg *msg)
{
	trace_apple_mbox_hw_send(apple_mbox, msg->msg, msg->info);
	WARN_ON(!apple_mbox_hw_can_send(apple_mbox));
	writeq(msg->msg, apple_mbox->regs + APPLE_IOP_A2I_MBOX_DATA);
	writeq(msg->info, apple_mbox->regs + APPLE_IOP_A2I_MBOX_INFO);
}

static bool apple_mbox_hw_can_recv(struct apple_mbox *apple_mbox)
{
	u32 mbox_ctrl = readl(apple_mbox->regs + APPLE_IOP_I2A_CONTROL);
	return !(mbox_ctrl & APPLE_IOP_I2A_CONTROL_EMPTY);
}

static void apple_mbox_hw_recv(struct apple_mbox *apple_mbox,
			       struct apple_mbox_msg *msg)
{
	WARN_ON(!apple_mbox_hw_can_recv(apple_mbox));
	msg->msg = readq(apple_mbox->regs + APPLE_IOP_I2A_MBOX_DATA);
	msg->info = readq(apple_mbox->regs + APPLE_IOP_I2A_MBOX_INFO);
	trace_apple_mbox_hw_recv(apple_mbox, msg->msg, msg->info);
}

static void apple_mbox_hw_cpu_enable(struct apple_mbox *apple_mbox)
{
	u32 cpu_control = readl(apple_mbox->regs + APPLE_IOP_CPU_CONTROL);
	cpu_control |= APPLE_IOP_CPU_CONTROL_RUN;
	writel(cpu_control, apple_mbox->regs + APPLE_IOP_CPU_CONTROL);
}

static bool apple_mbox_hw_cpu_is_enabled(struct apple_mbox *apple_mbox)
{
	u32 cpu_control = readl(apple_mbox->regs + APPLE_IOP_CPU_CONTROL);
	return !!(cpu_control & APPLE_IOP_CPU_CONTROL_RUN);
}

static void apple_rtkit_mgmnt_send_wakeup(struct apple_mbox *apple_mbox)
{
	struct apple_mbox_msg msg;
	msg.msg = APPLE_RTKIT_MGMT_WAKEUP;
	msg.info = APPLE_RTKIT_EP_MGMT;
	apple_mbox_hw_send(apple_mbox, &msg);
}

static struct mbox_chan *apple_mbox_init_chan(struct apple_mbox *apple_mbox,
					      u8 endpoint)
{
	int i, free_chan_idx;
	struct mbox_chan *chan;
	struct apple_chan_priv *chan_priv;
	struct mbox_controller *mbox = &apple_mbox->controller;

	free_chan_idx = -1;
	for (i = 0; i < mbox->num_chans; i++) {
		chan_priv = mbox->chans[i].con_priv;

		if (free_chan_idx < 0 && !chan_priv)
			free_chan_idx = i;
		if (chan_priv && chan_priv->endpoint == endpoint) {
			dev_err(mbox->dev, "Endpoint #0x%02d already in use.\n",
				endpoint);
			return ERR_PTR(-EBUSY);
		}
	}

	if (free_chan_idx < 0) {
		dev_err(mbox->dev, "No free channels left\n");
		return ERR_PTR(-EBUSY);
	}

	chan = &mbox->chans[free_chan_idx];
	chan_priv = devm_kzalloc(mbox->dev, sizeof(*chan_priv), GFP_KERNEL);
	if (!chan_priv)
		return ERR_PTR(-ENOMEM);

	chan_priv->endpoint = endpoint;
	chan_priv->apple_mbox = apple_mbox;
	chan->con_priv = chan_priv;
	return chan;
}

static struct mbox_chan *
apple_mbox_request_own_chan(struct apple_mbox *apple_mbox,
			    struct mbox_client *client, u8 endpoint)
{
	unsigned long flags;
	struct mbox_chan *chan = apple_mbox_init_chan(apple_mbox, endpoint);

	if (!chan)
		return chan;

	spin_lock_irqsave(&chan->lock, flags);
	chan->msg_free = 0;
	chan->msg_count = 0;
	chan->active_req = NULL;
	chan->cl = client;
	init_completion(&chan->tx_complete);
	spin_unlock_irqrestore(&chan->lock, flags);

	return chan;
}

static struct mbox_chan *apple_mbox_of_xlate(struct mbox_controller *mbox,
					     const struct of_phandle_args *spec)
{
	struct apple_mbox *apple_mbox = dev_get_drvdata(mbox->dev);
	int endpoint;

	if (spec->args_count != 1)
		return ERR_PTR(-EINVAL);

	endpoint = spec->args[0];

	if (apple_mbox->rtkit && endpoint < 0x20) {
		dev_err(mbox->dev,
			"RTKit system endpoints cannot be exposed\n");
		return ERR_PTR(-EINVAL);
	}

	return apple_mbox_init_chan(apple_mbox, endpoint);
}

static void apple_mbox_can_recv_irq_enable(struct apple_mbox *mbox, bool enable)
{
	trace_apple_mbox_can_recv_irq_enable(mbox, enable);
	if (enable)
		enable_irq(mbox->irq_can_recv);
	else
		disable_irq_nosync(mbox->irq_can_recv);
}

static void apple_mbox_can_send_irq_enable(struct apple_mbox *mbox, bool enable)
{
	trace_apple_mbox_can_send_irq_enable(mbox, enable);
	if (enable)
		enable_irq(mbox->irq_can_send);
	else
		disable_irq_nosync(mbox->irq_can_send);
}

static irqreturn_t apple_mbox_can_send_irq_handler(int irq, void *data)
{
	struct apple_mbox *apple_mbox = data;
	int i;

	apple_mbox_can_send_irq_enable(apple_mbox, false);

	/* just kick everything since all channels use the same hw fifo */
	for (i = 0;
	     i < APPLE_IOP_MAX_CHANS && apple_mbox_hw_can_send(apple_mbox); ++i)
		if (apple_mbox->chans[i].con_priv)
			mbox_chan_txdone(&apple_mbox->chans[i], 0);

	return IRQ_HANDLED;
}

static int apple_mbox_queue_msg(struct apple_mbox *apple_mbox,
				struct apple_mbox_msg *msg)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&apple_mbox->lock, flags);

	if (apple_mbox_hw_can_send(apple_mbox))
		apple_mbox_hw_send(apple_mbox, msg);
	else
		ret = -EBUSY;

	spin_unlock_irqrestore(&apple_mbox->lock, flags);
	return 0;
}

static irqreturn_t apple_mbox_recv_irq_handler(int irq, void *data)
{
	struct apple_mbox *mbox = data;
	struct apple_mbox_msg msg;
	bool wake = false;
	size_t len;
	unsigned long flags;

	while (apple_mbox_hw_can_recv(mbox)) {
		spin_lock_irqsave(&mbox->lock, flags);
		if (unlikely(kfifo_avail(&mbox->recv_fifo) < 1)) {
			apple_mbox_can_recv_irq_enable(mbox, false);
			mbox->recv_full = true;
			spin_unlock_irqrestore(&mbox->lock, flags);
			return IRQ_WAKE_THREAD;
		}
		spin_unlock_irqrestore(&mbox->lock, flags);

		apple_mbox_hw_recv(mbox, &msg);
		len = kfifo_in(&mbox->recv_fifo, &msg, 1);
		WARN_ON(len != 1);
		wake = true;
	}

	if (wake)
		return IRQ_WAKE_THREAD;
	else
		return IRQ_HANDLED;
}

static void shmem_dma_read(struct apple_mbox *apple_mbox, void *target,
			   struct apple_mbox_shared_memory bfr, off_t offset,
			   size_t size)
{
	memcpy(target, bfr.buffer + offset, size);
}

static void shmem_iobuf_read(struct apple_mbox *apple_mbox, void *target,
			     struct apple_mbox_shared_memory bfr, off_t offset,
			     size_t size)
{
	int i;
	u32 *target32 = (u32 *)target;

	WARN_ON(size % 4);
	for (i = 0; i < size / 4; ++i)
		target32[i] = readl(bfr.iomem + offset + 4 * i);
}

static void shmem_iobuf_handle_request(struct apple_mbox *apple_mbox,
				       struct mbox_chan *chan, u64 msg,
				       struct apple_mbox_shared_memory *buffer)
{
	buffer->size = FIELD_GET(APPLE_RTKIT_BUFFER_REQUEST_SIZE, msg) << 12;
	buffer->iova = FIELD_GET(APPLE_RTKIT_BUFFER_REQUEST_IOVA, msg);

	if (WARN_ON(!apple_mbox->mmio_shmem))
		goto error;

	/* 
	 * these are always bugs (or a rogue coprocessor firmware) and we can't
	 * do anything here to recover. this endpoint just won't work.
	 * (e.g. we won't get syslog messages or can't read crashlogs)
	 */
	if (buffer->iova < apple_mbox->mmio_shmem->start ||
	    buffer->iova > apple_mbox->mmio_shmem->end ||
	    buffer->iova + buffer->size < apple_mbox->mmio_shmem->start ||
	    buffer->iova + buffer->size > apple_mbox->mmio_shmem->end) {
		dev_warn(
			apple_mbox->dev,
			"coprocessor sent shmem buffer with 0x%zx bytes at 0x%llx outside of the configured region %pr",
			buffer->size, buffer->iova,
			apple_mbox->mmio_shmem);
		goto error;
	}

	buffer->iomem =
		devm_ioremap_np(apple_mbox->dev, buffer->iova, buffer->size);
	return;

error:
	buffer->size = 0;
	buffer->iova = 0;
}

static int __shmem_dma_handle_request(struct apple_mbox *apple_mbox, u64 msg,
				      u64 *reply,
				      struct apple_mbox_shared_memory *buffer)
{
	buffer->size = FIELD_GET(APPLE_RTKIT_BUFFER_REQUEST_SIZE, msg) << 12;
	buffer->buffer = dma_alloc_coherent(apple_mbox->dev, buffer->size,
					    &buffer->iova, GFP_KERNEL);
	if (!buffer->buffer) {
		dev_warn(
			apple_mbox->dev,
			"Cannot allocate shared memory buffer with size 0x%zx\n",
			buffer->size);
		return -ENOMEM;
	}

	*reply = FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, APPLE_RTKIT_BUFFER_REQUEST);
	*reply |=
		FIELD_PREP(APPLE_RTKIT_BUFFER_REQUEST_SIZE, buffer->size >> 12);
	*reply |= FIELD_PREP(APPLE_RTKIT_BUFFER_REQUEST_IOVA, buffer->iova);
	return 0;
}

static void shmem_dma_handle_request(struct apple_mbox *apple_mbox,
				     struct mbox_chan *chan, u64 msg,
				     struct apple_mbox_shared_memory *buffer)
{
	u64 reply;
	int ret;

	ret = __shmem_dma_handle_request(apple_mbox, msg, &reply, buffer);
	if (ret)
		return;

	ret = mbox_send_message(chan, (void *)reply);
	WARN_ON(ret < 0);
}

static void
shmem_sart_dma_handle_request(struct apple_mbox *apple_mbox,
			      struct mbox_chan *chan, u64 msg,
			      struct apple_mbox_shared_memory *buffer)
{
	int i, ret;
	u32 buffer_config;
	u64 reply;

	ret = __shmem_dma_handle_request(apple_mbox, msg, &reply, buffer);
	if (ret)
		return;

	if (WARN_ON(!apple_mbox->sart_regs))
		goto done;

	WARN_ON(buffer->size & ((1 << APPLE_SART_CONFIG_SIZE_SHIFT) - 1));
	WARN_ON(buffer->iova & ((1 << APPLE_SART_PADDR_SHIFT) - 1));

	buffer_config = FIELD_PREP(APPLE_SART_CONFIG_FLAGS, 0xff);
	buffer_config |=
		FIELD_PREP(APPLE_SART_CONFIG_SIZE,
			   buffer->size >> APPLE_SART_CONFIG_SIZE_SHIFT);

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; ++i) {
		u32 config =
			readl(apple_mbox->sart_regs + APPLE_SART_CONFIG(i));
		if (FIELD_GET(APPLE_SART_CONFIG_FLAGS, config) != 0)
			continue;

		writel(buffer->iova >> APPLE_SART_PADDR_SHIFT,
		       apple_mbox->sart_regs + APPLE_SART_PADDR(i));
		writel(buffer_config,
		       apple_mbox->sart_regs + APPLE_SART_CONFIG(i));
		break;
	}

	WARN_ON(i == APPLE_SART_MAX_ENTRIES);

done:
	ret = mbox_send_message(chan, (void *)reply);
	WARN_ON(ret < 0);
}

static void apple_rtkit_handle_msg_syslog_log(struct apple_mbox *apple_mbox,
					      u64 msg)
{
	u32 idx;
	u32 hdr, unk;
	int ret;
	char log_context[24];
	size_t entry_size = 0x20 + apple_mbox->syslog_msg_size;

	if (!apple_mbox->syslog_buffer.size) {
		dev_warn(apple_mbox->dev,
			 "received syslog message but have no syslog_buffer");
		goto done;
	}

	idx = msg & 0xff;
	if (idx > apple_mbox->syslog_n_entries) {
		dev_warn(apple_mbox->dev,
			 "syslog index #0x%x out of range (#0x%lx)", idx,
			 apple_mbox->syslog_n_entries);
		goto done;
	}

	if (!apple_mbox->syslog_msg_buffer) {
		dev_warn(apple_mbox->dev,
			 "received syslog message but no buffer available");
		goto done;
	}

	apple_mbox->shmem_ops->read(apple_mbox, &hdr, apple_mbox->syslog_buffer,
				    idx * entry_size, 4);
	apple_mbox->shmem_ops->read(apple_mbox, &unk, apple_mbox->syslog_buffer,
				    idx * entry_size + 4, 4);
	apple_mbox->shmem_ops->read(apple_mbox, log_context,
				    apple_mbox->syslog_buffer,
				    idx * entry_size + 8, sizeof(log_context));
	apple_mbox->shmem_ops->read(apple_mbox, apple_mbox->syslog_msg_buffer,
				    apple_mbox->syslog_buffer,
				    idx * entry_size + 8 + sizeof(log_context),
				    apple_mbox->syslog_msg_size);

	log_context[sizeof(log_context) - 1] = 0;
	apple_mbox->syslog_msg_buffer[apple_mbox->syslog_msg_size - 1] = 0;
	dev_info(apple_mbox->dev, "syslog message: %s: %s", log_context,
		 apple_mbox->syslog_msg_buffer);

done:
	ret = mbox_send_message(apple_mbox->syslog_chan, (void *)msg);
	WARN_ON(ret < 0);
}

void apple_rtkit_syslog_rx_callback(struct mbox_client *cl, void *mssg)
{
	struct apple_mbox *apple_mbox = dev_get_drvdata(cl->dev);
	struct mbox_controller *mbox = &apple_mbox->controller;
	u64 msg = (u64)mssg;
	u8 type = FIELD_GET(APPLE_RTKIT_MGMT_TYPE, msg);

	switch (type) {
	case APPLE_RTKIT_BUFFER_REQUEST:
		apple_mbox->shmem_ops->handle_request(
			apple_mbox, apple_mbox->syslog_chan, msg,
			&apple_mbox->syslog_buffer);
		break;
	case APPLE_RTKIT_SYSLOG_INIT:
		apple_mbox->syslog_n_entries =
			FIELD_GET(APPLE_RTKIT_SYSLOG_N_ENTRIES, msg);
		apple_mbox->syslog_msg_size =
			FIELD_GET(APPLE_RTKIT_SYSLOG_MSG_SIZE, msg);
		apple_mbox->syslog_msg_buffer =
			devm_kzalloc(apple_mbox->dev,
				     apple_mbox->syslog_msg_size, GFP_KERNEL);
		if (!apple_mbox->syslog_msg_buffer)
			dev_warn(apple_mbox->dev,
				 "Unable to allocate syslog buffer");
		break;
	case APPLE_RTKIT_SYSLOG_LOG:
		apple_rtkit_handle_msg_syslog_log(apple_mbox, msg);
		break;
	default:
		dev_warn(mbox->dev, "received message 0x%016llx for syslog ep",
			 msg);
		break;
	}
}

static void apple_rtkit_init_syslog(struct apple_mbox *apple_mbox)
{
	apple_mbox->syslog_client.dev = apple_mbox->dev;
	apple_mbox->syslog_client.rx_callback = &apple_rtkit_syslog_rx_callback;
	apple_mbox->syslog_client.tx_block = false;
	apple_mbox->syslog_client.knows_txdone = false;
	apple_mbox->syslog_chan = apple_mbox_request_own_chan(
		apple_mbox, &apple_mbox->syslog_client, APPLE_RTKIT_EP_SYSLOG);
}

static void apple_rtkit_crashlog_rx_callback(struct mbox_client *cl, void *mssg)
{
	struct apple_mbox *apple_mbox = dev_get_drvdata(cl->dev);
	struct mbox_controller *mbox = &apple_mbox->controller;
	struct apple_rtkit_crashlog_header crashlog_header;
	struct debugfs_blob_wrapper *debugfs_blob;
	size_t crashlog_size;
	u64 msg = (u64)mssg;
	u8 type = FIELD_GET(APPLE_RTKIT_MGMT_TYPE, msg);
	u8 crashlog_name[16];
	u8 *buffer;

	if (type != APPLE_RTKIT_BUFFER_REQUEST) {
		dev_warn(mbox->dev,
			 "received unknown message 0x%016llx for crashlog ep",
			 msg);
		return;
	}

	if (!apple_mbox->crashlog_buffer.size) {
		apple_mbox->shmem_ops->handle_request(
			apple_mbox, apple_mbox->crashlog_chan, msg,
			&apple_mbox->crashlog_buffer);
		return;
	}

	dev_err(mbox->dev, "coprocessor has sent a crashlog.");

	if (!apple_mbox->crashlog_buffer.size) {
		dev_warn(
			apple_mbox->dev,
			"received crashlog message but have no crashlog_buffer");
		return;
	}

	apple_mbox->shmem_ops->read(apple_mbox, &crashlog_header,
				    apple_mbox->crashlog_buffer, 0,
				    sizeof(crashlog_header));
	if (crashlog_header.magic == APPLE_RTKIT_CRASHLOG_HEADER_MAGIC &&
	    crashlog_header.size <= apple_mbox->crashlog_buffer.size)
		crashlog_size = crashlog_header.size;
	else
		crashlog_size = apple_mbox->crashlog_buffer.size;

	buffer = devm_kzalloc(apple_mbox->dev, crashlog_size, GFP_KERNEL);
	if (!buffer) {
		dev_err(mbox->dev, "couldn't allocate buffer for crashlog.");
		return;
	}

	debugfs_blob = devm_kzalloc(apple_mbox->dev, sizeof(*debugfs_blob),
				    GFP_KERNEL);
	if (!debugfs_blob) {
		dev_err(mbox->dev,
			"couldn't allocate buffer for debugfs_blob.");
		devm_kfree(mbox->dev, buffer);
		return;
	}

	apple_mbox->shmem_ops->read(apple_mbox, buffer,
				    apple_mbox->crashlog_buffer, 0,
				    crashlog_size);

	debugfs_blob->data = buffer;
	debugfs_blob->size = crashlog_size;
	snprintf(crashlog_name, sizeof(crashlog_name), "crashlog.%d",
		 apple_mbox->crashlog_idx);
	apple_mbox->crashlog_idx++;
	debugfs_create_blob(crashlog_name, S_IRUGO, apple_mbox->debugfs_root,
			    debugfs_blob);
}

static void apple_rtkit_init_crashlog(struct apple_mbox *apple_mbox)
{
	apple_mbox->crashlog_client.dev = apple_mbox->dev;
	apple_mbox->crashlog_client.rx_callback =
		&apple_rtkit_crashlog_rx_callback;
	apple_mbox->crashlog_client.tx_block = false;
	apple_mbox->crashlog_client.knows_txdone = false;
	apple_mbox->crashlog_chan =
		apple_mbox_request_own_chan(apple_mbox,
					    &apple_mbox->crashlog_client,
					    APPLE_RTKIT_EP_CRASHLOG);
}

static void apple_rtkit_ioreport_rx_callback(struct mbox_client *cl, void *mssg)
{
	struct apple_mbox *apple_mbox = dev_get_drvdata(cl->dev);
	struct mbox_controller *mbox = &apple_mbox->controller;
	u64 msg = (u64)mssg;
	u8 type = FIELD_GET(APPLE_RTKIT_MGMT_TYPE, msg);
	int ret;

	switch (type) {
	case APPLE_RTKIT_BUFFER_REQUEST:
		apple_mbox->shmem_ops->handle_request(
			apple_mbox, apple_mbox->ioreport_chan, msg,
			&apple_mbox->ioreport_buffer);
		break;
	// unknown messages, but must be ACKed
	case 0x8:
	case 0xc:
		ret = mbox_send_message(apple_mbox->ioreport_chan, (void *)msg);
		WARN_ON(ret < 0);
		break;
	default:
		dev_warn(mbox->dev,
			 "received unknown message 0x%016llx for ioreport ep",
			 msg);
		break;
	}
}

static void apple_rtkit_init_ioreport(struct apple_mbox *apple_mbox)
{
	apple_mbox->ioreport_client.dev = apple_mbox->dev;
	apple_mbox->ioreport_client.rx_callback =
		&apple_rtkit_ioreport_rx_callback;
	apple_mbox->ioreport_client.tx_block = false;
	apple_mbox->ioreport_client.knows_txdone = false;
	apple_mbox->ioreport_chan =
		apple_mbox_request_own_chan(apple_mbox,
					    &apple_mbox->ioreport_client,
					    APPLE_RTKIT_EP_IOREPORT);
}

static void apple_rtkit_management_rx_callback(struct mbox_client *cl,
					       void *mssg)
{
	struct apple_mbox *apple_mbox = dev_get_drvdata(cl->dev);
	struct mbox_controller *mbox = &apple_mbox->controller;
	u64 msg = (u64)mssg;
	u8 type = FIELD_GET(APPLE_RTKIT_MGMT_TYPE, msg);
	int i, ep, ret;
	u64 reply;

	switch (type) {
	case APPLE_RTKIT_MGMT_HELLO:
		reply = FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_TAG,
				   FIELD_GET(APPLE_RTKIT_MGMT_HELLO_TAG, msg));
		reply |= FIELD_PREP(APPLE_RTKIT_MGMT_TYPE,
				    APPLE_RTKIT_MGMT_HELLO_REPLY);
		ret = mbox_send_message(apple_mbox->management_chan,
					(void *)reply);
		WARN_ON(ret < 0);
		break;
	case APPLE_RTKIT_MGMT_EPMAP:
		for (i = 0; i < 32; ++i) {
			u32 bitmap =
				FIELD_GET(APPLE_RTKIT_MGMT_EPMAP_BITMAP, msg);
			u32 base = FIELD_GET(APPLE_RTKIT_MGMT_EPMAP_BASE, msg);
			if (bitmap & BIT(i))
				set_bit(32 * base + i,
					apple_mbox->rtkit_endpoints);
		}

		reply = FIELD_PREP(APPLE_RTKIT_MGMT_TYPE,
				   APPLE_RTKIT_MGMT_EPMAP_REPLY);
		reply |=
			FIELD_PREP(APPLE_RTKIT_MGMT_EPMAP_BASE,
				   FIELD_GET(APPLE_RTKIT_MGMT_EPMAP_BASE, msg));
		if (msg & APPLE_RTKIT_MGMT_EPMAP_LAST)
			reply |= APPLE_RTKIT_MGMT_EPMAP_LAST;
		else
			reply |= APPLE_RTKIT_MGMT_EPMAP_REPLY_MORE;
		ret = mbox_send_message(apple_mbox->management_chan,
					(void *)reply);
		WARN_ON(ret < 0);

		if (msg & APPLE_RTKIT_MGMT_EPMAP_LAST) {
			for_each_set_bit (ep, apple_mbox->rtkit_endpoints,
					  0x100) {
				if (ep == 0)
					continue;
				reply = FIELD_PREP(APPLE_RTKIT_MGMT_TYPE,
						   APPLE_RTKIT_MGMT_STARTEP);
				reply |= FIELD_PREP(APPLE_RTKIT_MGMT_STARTEP_EP,
						    ep);
				reply |= APPLE_RTKIT_MGMT_STARTEP_FLAG;
				ret = mbox_send_message(
					apple_mbox->management_chan,
					(void *)reply);
				WARN_ON(ret <= 0);
			}
		}
		break;
	case APPLE_RTKIT_MGMT_BOOT_DONE:
		reply = FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, 0xb);
		reply |= FIELD_PREP(APPLE_RTKIT_MGMT_BOOT_DONE_UNK, 0x20);
		ret = mbox_send_message(apple_mbox->management_chan,
					(void *)reply);
		WARN_ON(ret < 0);
		break;
	case APPLE_RTKIT_MGMT_BOOT_DONE2:
		complete_all(&apple_mbox->ready_completion);
		dev_info(mbox->dev,
			 "RTKit system endpoints successfuly initialized!");
		break;
	default:
		dev_warn(mbox->dev,
			 "received unknown message 0x%016llx for management ep",
			 msg);
		break;
	}
}

static void apple_rtkit_init_management(struct apple_mbox *apple_mbox)
{
	apple_mbox->management_client.dev = apple_mbox->dev;
	apple_mbox->management_client.rx_callback =
		&apple_rtkit_management_rx_callback;
	apple_mbox->management_client.tx_block = false;
	apple_mbox->management_client.knows_txdone = false;
	apple_mbox->management_chan =
		apple_mbox_request_own_chan(apple_mbox,
					    &apple_mbox->management_client,
					    APPLE_RTKIT_EP_MGMT);
}

static irqreturn_t apple_mbox_recv_irq_thread(int irq, void *data)
{
	struct apple_mbox *apple_mbox = data;
	struct mbox_controller *mbox = &apple_mbox->controller;
	struct apple_mbox_msg msg;
	struct apple_chan_priv *chan_priv;
	int i;
	u8 endpoint;
	size_t len;
	bool found;
	unsigned long flags;

	while (kfifo_len(&apple_mbox->recv_fifo) >= 1) {
		len = kfifo_out(&apple_mbox->recv_fifo, &msg, 1);
		WARN_ON(len != 1);

		endpoint = msg.info & 0xff;

		found = false;
		for (i = 0; i < mbox->num_chans && !found; i++) {
			chan_priv = mbox->chans[i].con_priv;
			if (chan_priv && chan_priv->endpoint == endpoint) {
				mbox_chan_received_data(&mbox->chans[i],
							(void *)msg.msg);
				found = true;
			}
		}

		if (!found)
			dev_err(mbox->dev,
				"Received message for unknown endpoint #0x%02x.",
				endpoint);

		spin_lock_irqsave(&apple_mbox->lock, flags);
		if (apple_mbox->recv_full) {
			apple_mbox->recv_full = false;
			apple_mbox_can_recv_irq_enable(apple_mbox, true);
		}
		spin_unlock_irqrestore(&apple_mbox->lock, flags);
	}

	return IRQ_HANDLED;
}

static int apple_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct apple_chan_priv *chan_priv = chan->con_priv;
	struct apple_mbox *apple_mbox = chan_priv->apple_mbox;
	struct apple_mbox_msg msg;
	int ret;

	msg.info = chan_priv->endpoint;
	msg.msg = (u64)data;

	ret = apple_mbox_queue_msg(apple_mbox, &msg);
	apple_mbox_can_send_irq_enable(apple_mbox, true);

	return ret;
}

static int apple_mbox_startup(struct mbox_chan *chan)
{
	struct apple_chan_priv *chan_priv = chan->con_priv;
	struct apple_mbox *apple_mbox = chan_priv->apple_mbox;

	wait_for_completion(&apple_mbox->ready_completion);

	return 0;
}

static const struct mbox_chan_ops apple_mbox_ops = {
	.send_data = &apple_mbox_send_data,
	.startup = &apple_mbox_startup,
};

static const struct of_device_id apple_mbox_of_match[];

static int apple_mbox_probe(struct platform_device *pdev)
{
	int ret;
	struct apple_mbox *mbox;
	struct resource *regs;
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	const struct apple_mailbox_private *match_data;

	match = of_match_node(apple_mbox_of_match, pdev->dev.of_node);
	if (!match)
		return -EINVAL;
	match_data = match->data;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	platform_set_drvdata(pdev, mbox);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	mbox->dev = dev;
	spin_lock_init(&mbox->lock);
	INIT_KFIFO(mbox->recv_fifo);
	init_completion(&mbox->ready_completion);

	regs = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mbox");
	if (!regs)
		return -EINVAL;

	mbox->regs = devm_ioremap_resource(dev, regs);
	if (IS_ERR(mbox->regs))
		return PTR_ERR(mbox->regs);

	/* DMA allowlist required for some coprocessors */
	if (match_data->require_sart) {
		regs = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						    "sart");
		if (!regs)
			return -EINVAL;
		mbox->sart_regs = devm_ioremap_resource(dev, regs);
		if (IS_ERR(mbox->sart_regs))
			return PTR_ERR(mbox->sart_regs);
	}

	/*
	 * some coprocessors expose shared memory over the MMIO bus.
	 * this memory is not mapped here, the resource is used to verify
	 * incoming pointers (see e.g. shmem_iobuf_handle_request)
	 */
	if (match_data->require_shmem) {
		mbox->mmio_shmem = platform_get_resource_byname(
			pdev, IORESOURCE_MEM, "shmem");
		if (!mbox->mmio_shmem)
			return -EINVAL;
	}

	mbox->irq_can_send = platform_get_irq_byname(pdev, "can-send");
	if (mbox->irq_can_send < 0)
		return -ENODEV;
	mbox->irq_can_recv = platform_get_irq_byname(pdev, "can-recv");
	if (mbox->irq_can_recv < 0)
		return -ENODEV;

	ret = devm_clk_bulk_get_all(dev, &mbox->clks);
	if (ret)
		return ret;
	mbox->num_clks = ret;

	ret = clk_bulk_prepare_enable(mbox->num_clks, mbox->clks);
	if (ret)
		return ret;

	mbox->debugfs_root = debugfs_create_dir(dev_name(dev), NULL);
	if (!mbox->debugfs_root) {
		ret = PTR_ERR(mbox->debugfs_root);
		goto err_clk_disable;
	}

	mbox->controller.dev = mbox->dev;
	mbox->controller.num_chans = APPLE_IOP_MAX_CHANS;
	mbox->controller.chans = mbox->chans;
	mbox->controller.ops = &apple_mbox_ops;
	mbox->controller.of_xlate = &apple_mbox_of_xlate;
	mbox->rtkit = match_data->rtkit;
	mbox->shmem_ops = match_data->shmem_ops;
	mbox->controller.txdone_irq = true;

	ret = request_irq(mbox->irq_can_send, &apple_mbox_can_send_irq_handler,
			  0, dev_name(dev), mbox);
	if (ret)
		goto err_debugfs_remove;

	ret = request_threaded_irq(mbox->irq_can_recv,
				   &apple_mbox_recv_irq_handler,
				   &apple_mbox_recv_irq_thread, 0,
				   dev_name(dev), mbox);
	if (ret)
		goto free_can_send_irq;

	ret = devm_mbox_controller_register(dev, &mbox->controller);
	if (ret)
		goto free_can_recv_irq;

	apple_rtkit_init_syslog(mbox);
	apple_rtkit_init_crashlog(mbox);
	apple_rtkit_init_ioreport(mbox);
	apple_rtkit_init_management(mbox);

	if (mbox->rtkit) {
		if (apple_mbox_hw_cpu_is_enabled(mbox))
			apple_rtkit_mgmnt_send_wakeup(mbox);
		else
			apple_mbox_hw_cpu_enable(mbox);
	} else {
		apple_mbox_hw_cpu_enable(mbox);
		complete_all(&mbox->ready_completion);
	}

	return ret;

free_can_recv_irq:
	free_irq(mbox->irq_can_recv, mbox);
free_can_send_irq:
	free_irq(mbox->irq_can_send, mbox);
err_debugfs_remove:
	debugfs_remove_recursive(mbox->debugfs_root);
err_clk_disable:
	clk_bulk_disable_unprepare(mbox->num_clks, mbox->clks);
	return ret;
}

static const struct apple_mbox_shmem_ops apple_mbox_shmem_dma_ops = {
	.handle_request = &shmem_dma_handle_request,
	.read = &shmem_dma_read
};

static const struct apple_mbox_shmem_ops apple_mbox_shmem_sart_dma_ops = {
	.handle_request = &shmem_sart_dma_handle_request,
	.read = &shmem_dma_read
};

static const struct apple_mbox_shmem_ops apple_mbox_shmem_iomem_ops = {
	.handle_request = &shmem_iobuf_handle_request,
	.read = &shmem_iobuf_read
};

static const struct apple_mailbox_private apple_smc_mbox_data = {
	.rtkit = true,
	.require_shmem = true,
	.shmem_ops = &apple_mbox_shmem_iomem_ops,
};

static const struct apple_mailbox_private apple_ans_mbox_data = {
	.rtkit = true,
	.require_sart = true,
	.shmem_ops = &apple_mbox_shmem_sart_dma_ops,
};

static const struct apple_mailbox_private apple_rtkit_mbox_data = {
	.rtkit = true,
	.shmem_ops = &apple_mbox_shmem_dma_ops,
};

static const struct apple_mailbox_private apple_base_mbox_data = {
	.rtkit = false,
};

static const struct of_device_id apple_mbox_of_match[] = {
	{ .compatible = "apple,t8103-ans-mailbox",
	  .data = &apple_ans_mbox_data },
	{ .compatible = "apple,t8103-smc-mailbox",
	  .data = &apple_smc_mbox_data },
	{ .compatible = "apple,t8103-rtkit-mailbox",
	  .data = &apple_rtkit_mbox_data },
	{ .compatible = "apple,t8103-sepos-mailbox",
	  .data = &apple_base_mbox_data },
	{},
};
MODULE_DEVICE_TABLE(of, apple_mbox_of_match);


static int apple_mbox_remove(struct platform_device *pdev)
{
	struct apple_mbox *apple_mbox = platform_get_drvdata(pdev);
	struct apple_mbox_msg msg;

	free_irq(apple_mbox->irq_can_recv, apple_mbox);
	free_irq(apple_mbox->irq_can_send, apple_mbox);

	while (apple_mbox_hw_can_recv(apple_mbox)) {
		apple_mbox_hw_recv(apple_mbox, &msg);
		dev_warn(apple_mbox->dev, "discarding message %llx / %llx during shutdown.", msg.msg, msg.info);
	}

	msg.info = 0;
	msg.msg = 0x00b0000000000010;
	if (apple_mbox_hw_can_send(apple_mbox))
		apple_mbox_hw_send(apple_mbox, &msg);

	msg.info = 0;
	msg.msg = 0x0060000000000010;
	if (apple_mbox_hw_can_send(apple_mbox))
		apple_mbox_hw_send(apple_mbox, &msg);

	/*
	 * we could check replies here but there's not much we could do
	 * if anything unexpected happens. let's just discard them
	 * so that whatever runs after us isn't confused.
	 */
	while (apple_mbox_hw_can_recv(apple_mbox))
		apple_mbox_hw_recv(apple_mbox, &msg);

	return 0;
}

static void apple_mbox_shutdown(struct platform_device *pdev)
{
	apple_mbox_remove(pdev);
}

static struct platform_driver apple_mbox_driver = {
	.driver = {
		.name = "apple-mailbox",
		.of_match_table = apple_mbox_of_match,
	},
	.probe = apple_mbox_probe,
	.remove = apple_mbox_remove,
	.shutdown = apple_mbox_shutdown,
};
module_platform_driver(apple_mbox_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_DESCRIPTION("Apple mailbox driver");
