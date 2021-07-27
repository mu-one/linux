#ifndef APPLE_ASC_MAILBOX
#define APPLE_ASC_MAILBOX 1

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kfifo.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/types.h>

/* SART DMA allow list for shared memory buffers */
#define APPLE_SART_CONFIG(idx) (0x00 + 4 * (idx))
#define APPLE_SART_CONFIG_FLAGS GENMASK(31, 24)
#define APPLE_SART_CONFIG_SIZE GENMASK(23, 0)
#define APPLE_SART_CONFIG_SIZE_SHIFT 12

#define APPLE_SART_PADDR(idx) (0x40 + 4 * (idx))
#define APPLE_SART_PADDR_SHIFT 12

#define APPLE_SART_MAX_ENTRIES 16

/* A2I = Application Processor (us) to I/O Processor (usually RTKit) */

#define APPLE_IOP_CPU_CONTROL 0x44
#define APPLE_IOP_CPU_CONTROL_RUN 0x10

#define APPLE_IOP_A2I_CONTROL 0x8110
#define APPLE_IOP_A2I_CONTROL_FULL BIT(16)
#define APPLE_IOP_A2I_CONTROL_EMPTY BIT(17)

#define APPLE_IOP_I2A_CONTROL 0x8114
#define APPLE_IOP_I2A_CONTROL_FULL BIT(16)
#define APPLE_IOP_I2A_CONTROL_EMPTY BIT(17)

#define APPLE_IOP_A2I_MBOX_DATA 0x8800
#define APPLE_IOP_A2I_MBOX_INFO 0x8808
#define APPLE_IOP_I2A_MBOX_DATA 0x8830
#define APPLE_IOP_I2A_MBOX_INFO 0x8838

#define APPLE_RTKIT_EP_MGMT 0
#define APPLE_RTKIT_MGMT_WAKEUP 0x60000000000020

#define APPLE_RTKIT_EP_CRASHLOG 1
#define APPLE_RTKIT_EP_SYSLOG 2
#define APPLE_RTKIT_EP_DEBUG 3
#define APPLE_RTKIT_EP_IOREPORT 4

#define APPLE_RTKIT_MGMT_TYPE GENMASK(59, 52)

#define APPLE_RTKIT_MGMT_HELLO 1
#define APPLE_RTKIT_MGMT_HELLO_REPLY 2
#define APPLE_RTKIT_MGMT_HELLO_TAG GENMASK(31, 0)

#define APPLE_RTKIT_MGMT_EPMAP 8
#define APPLE_RTKIT_MGMT_EPMAP_LAST BIT(51)
#define APPLE_RTKIT_MGMT_EPMAP_BASE GENMASK(34, 32)
#define APPLE_RTKIT_MGMT_EPMAP_BITMAP GENMASK(31, 0)

#define APPLE_RTKIT_MGMT_EPMAP_REPLY 8
#define APPLE_RTKIT_MGMT_EPMAP_REPLY_MORE BIT(0)

#define APPLE_RTKIT_MGMT_STARTEP 5
#define APPLE_RTKIT_MGMT_STARTEP_EP GENMASK(39, 32)
#define APPLE_RTKIT_MGMT_STARTEP_FLAG BIT(1)

#define APPLE_RTKIT_MGMT_BOOT_DONE 7
#define APPLE_RTKIT_MGMT_BOOT_DONE_UNK GENMASK(15, 0)

#define APPLE_RTKIT_MGMT_BOOT_DONE2 0xb

#define APPLE_RTKIT_BUFFER_REQUEST 1
#define APPLE_RTKIT_BUFFER_REQUEST_SIZE GENMASK(51, 44)
#define APPLE_RTKIT_BUFFER_REQUEST_IOVA GENMASK(39, 0)

#define APPLE_RTKIT_SYSLOG_LOG 5

#define APPLE_RTKIT_SYSLOG_INIT 8
#define APPLE_RTKIT_SYSLOG_N_ENTRIES GENMASK(7, 0)
#define APPLE_RTKIT_SYSLOG_MSG_SIZE GENMASK(31, 24)

#define APPLE_RTKIT_CRASHLOG_HEADER_MAGIC 0x434C4845

/* max channels to save memory; IPC protocol supports up to 0x100 chans */
#define APPLE_IOP_MAX_CHANS 20
#define APPLE_IOP_MAX2_CHANS 0x100

struct apple_mbox;
struct apple_mbox_shmem_ops;

struct apple_rtkit_crashlog_header {
	u32 magic;
	u32 unk;
	u32 size;
	u32 flags;
	u8 padding[0x10];
};

struct apple_mailbox_private {
	bool rtkit;
	const struct apple_mbox_shmem_ops *shmem_ops;
	bool require_sart;
	bool require_shmem;
};

struct apple_chan_priv {
	u8 endpoint;
	struct apple_mbox *apple_mbox;
};

struct apple_mbox_msg {
	u64 msg;
	u64 info;
};

struct apple_mbox_shared_memory {
	union {
		void *buffer;
		void __iomem *iomem;
	};
	size_t size;
	dma_addr_t iova;
};

struct apple_mbox_shmem_ops {
	void (*handle_request)(struct apple_mbox *, struct mbox_chan *, u64,
			       struct apple_mbox_shared_memory *);
	void (*read)(struct apple_mbox *, void *,
		     struct apple_mbox_shared_memory, off_t, size_t);
	//void (*free)(struct apple_mbox *, struct apple_mbox_shared_memory);
};

struct apple_mbox {
	void __iomem *regs, *sart_regs;
	struct resource *mmio_shmem;
	int irq_can_send, irq_can_recv;

	struct clk_bulk_data *clks;
	int num_clks;

	struct mbox_chan chans[APPLE_IOP_MAX_CHANS];
	struct completion ready_completion;

	bool rtkit;
	DECLARE_BITMAP(rtkit_endpoints, 0x100);

	const struct apple_mbox_shmem_ops *shmem_ops;

	struct mbox_client syslog_client;
	struct mbox_chan *syslog_chan;
	struct apple_mbox_shared_memory syslog_buffer;
	char *syslog_msg_buffer;
	size_t syslog_n_entries;
	size_t syslog_msg_size;

	struct mbox_client crashlog_client;
	struct mbox_chan *crashlog_chan;
	struct apple_mbox_shared_memory crashlog_buffer;
	u32 crashlog_idx;

	struct mbox_client ioreport_client;
	struct mbox_chan *ioreport_chan;
	struct apple_mbox_shared_memory ioreport_buffer;

	struct mbox_client management_client;
	struct mbox_chan *management_chan;

	DECLARE_KFIFO(recv_fifo, struct apple_mbox_msg, 16);
	bool recv_full;

	struct dentry *debugfs_root;

	spinlock_t lock;

	struct device *dev;
	struct mbox_controller controller;
};

#endif