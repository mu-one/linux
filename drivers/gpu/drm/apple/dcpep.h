// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io> */

#ifndef __APPLE_DCPEP_H__
#define __APPLE_DCPEP_H__

/* Endpoint for general DCP traffic (dcpep in macOS) */
#define DCP_ENDPOINT 0x37

/* Fixed size of shared memory between DCP and AP */
#define DCP_SHMEM_SIZE 0x100000

/* DCP message contexts */
enum dcp_context_id {
	/* Callback */
	DCP_CONTEXT_CB = 0,

	/* Command */
	DCP_CONTEXT_CMD = 2,

	/* Asynchronous */
	DCP_CONTEXT_ASYNC = 3,

	/* Out-of-band callback */
	DCP_CONTEXT_OOBCB = 4,

	/* Out-of-band command */
	DCP_CONTEXT_OOBCMD = 5,
};

/* RTKit endpoint message types */
enum dcpep_type {
	/* Set shared memory */
	DCPEP_TYPE_SET_SHMEM = 0,

	/* DCP is initialized */
	DCPEP_TYPE_INITIALIZED = 1,

	/* Remote procedure call */
	DCPEP_TYPE_MESSAGE = 2,
};

/* Message */
#define DCPEP_TYPE_SHIFT (0)
#define DCPEP_TYPE_MASK GENMASK(1, 0)
#define DCPEP_ACK_SHIFT (6)
#define DCPEP_CONTEXT_SHIFT (8)
#define DCPEP_CONTEXT_MASK GENMASK(11, 8)
#define DCPEP_OFFSET_SHIFT (16)
#define DCPEP_OFFSET_MASK GENMASK(31, 16)
#define DCPEP_LENGTH_SHIFT (32)

/* Set shmem */
#define DCPEP_DVA_SHIFT (16)
#define DCPEP_FLAG_SHIFT (4)
#define DCPEP_FLAG_VALUE (4)

struct apple_dcp {
	struct device *dev;
	struct apple_rtkit *rtk;

	/* DCP shared memory */
	void *shmem;
	dma_addr_t shmem_iova;
};

struct dcp_context {
	uint8_t *buf;
	uint16_t offset;
	enum dcp_context_id id;
};

struct dcp_packet_header {
	char tag[4];
	uint32_t in_len;
	uint32_t out_len;
} __attribute__((packed));

#define DCP_IS_NULL(ptr) ((ptr) ? 1 : 0)
#define DCP_PACKET_ALIGNMENT (0x40)

static inline uint64_t
dcpep_set_shmem(uint64_t dart_va)
{
	return (DCPEP_TYPE_SET_SHMEM << DCPEP_TYPE_SHIFT) |
		(DCPEP_FLAG_VALUE << DCPEP_FLAG_SHIFT) |
		(dart_va << DCPEP_DVA_SHIFT);
}

static inline uint64_t
dcpep_msg(enum dcp_context_id id, uint32_t length, uint16_t offset, bool ack)
{
	return (DCPEP_TYPE_MESSAGE << DCPEP_TYPE_SHIFT) |
		(ack ? BIT_ULL(DCPEP_ACK_SHIFT) : 0) |
		((uint64_t) id << DCPEP_CONTEXT_SHIFT) |
		((uint64_t) offset << DCPEP_OFFSET_SHIFT) |
		((uint64_t) length << DCPEP_LENGTH_SHIFT);
}

#endif
