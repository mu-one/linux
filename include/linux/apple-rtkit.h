#ifndef _LINUX_APPLE_RTKIT_H_
#define _LINUX_APPLE_RTKIT_H_

#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/types.h>
#include <linux/mailbox_client.h>

/*
 * Enum to identify if Linux or RTKit own the shared memory buffers.
 */
enum {
	APPLE_RTKIT_SHMEM_OWNER_LINUX = 0,
	APPLE_RTKIT_SHMEM_OWNER_RTKIT = 1,
};

/*
 * Struct to represent implementation-specific RTKit operations.
 *
 * @recv_message: Function called when a message from RTKit is recevied
 *                on a non-system endpoint.
 * @shmem_owner:  Specified the owner of the shared memory buffers, can
 *                be either APPLE_RTKIT_SHMEM_OWNER_LINUX or
 *                APPLE_RTKIT_SHMEM_OWNER_RTKIT
 * @shmem_verify: If the shared memory buffers reside in an MMIO region
 *                and are owned by the co-processor this function is
 *                called to verify each buffer sent by the co-processor.
 * @shmem_alloc:  If the shared memory buffers are managed by us
 *                this function is called to allocate a buffer. If no
 *                function is given dma_alloc_coherent is used.
 * @shmem_free:   If the shared memory buffers are managed by us
 *                this function is called to free the given buffer. If
 *                no function is given dma_free_coherent is used.
 */
struct apple_rtkit_ops {
	void (*recv_message)(void *cookie, u8 endpoint, u64 message);
	unsigned int shmem_owner;
	int (*shmem_verify)(void *cookie, dma_addr_t addr, size_t len);
	void *(*shmem_alloc)(void *cookie, size_t size, dma_addr_t *dma_handle,
			    gfp_t flag);
	void (*shmem_free)(void *cookie, size_t size, void *cpu_addr,
			   dma_addr_t *dma_handle);
};

struct apple_rtkit;

/*
 * Initializes the internal state required to handle RTKit. This
 * should usually be called withint _probe.
 *
 * @dev: Pointer to the device node this coprocessor is assocated with
 * @cookie: opaque cookie passed to all functions defined in rtkit_ops
 * @resource: resource containing the CPU_CONTROL register
 * @mbox_name: mailbox name used to communicate with the co-processor
 * @ops: pointer to rtkit_ops to be used for this co-processor
 */
struct apple_rtkit *apple_rtkit_init(struct device *dev, void *cookie,
				     struct resource *res,
				     const char *mbox_name,
				     const struct apple_rtkit_ops *ops);

void apple_rtkit_free(struct apple_rtkit *rtk);

/*
 * Turns on the co-processor and initialize the RTKit system endpoints.
 * Has to be called before any messages can be sent or recevied.
 */
int apple_rtkit_boot(struct apple_rtkit *rtk, struct completion *boot_done);

/*
 * Same as rtkit_boot but waits until the processor has booted successfully.
 */
int apple_rtkit_boot_wait(struct apple_rtkit *rtk);

/*
 * Puts the co-processor into hibernation mode.
 * The processor loses almost all state and cannot be used anymore after this
 * call.
 */
int apple_rtkit_hibernate(struct apple_rtkit *rtk);

/*
 * Starts an endpoint. Must be called after boot but before any messages can be
 * sent or received from that endpoint.
 */
int apple_rtkit_start_ep(struct apple_rtkit *rtk, u8 endpoint);

/*
 * Send a message to the given endpoint.
 */
int apple_rtkit_send_message(struct apple_rtkit *rtk, u8 ep, u64 message);

#endif