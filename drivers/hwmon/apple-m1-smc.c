// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Apple SMC as found on M1 SoC
 *
 * Copyright (C) 2021 Corellium LLC
 */

#include <linux/apple-mailbox.h>
#include <linux/apple-rtkit.h>
#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>

#define MAX_GPIO		32
#define SMC_ENDPOINT		0x20

#define SMC_READ_KEY		0x10
#define SMC_WRITE_KEY		0x11
#define SMC_GET_KEY_BY_INDEX	0x12
#define SMC_GET_KEY_INFO	0x13
#define SMC_GET_SRAM_ADDR	0x17
#define SMC_NOTIFICATION	0x18
#define SMC_READ_KEY_PAYLOAD	0x20

#define SMC_BUF_SIZE		0x4000
#define SMC_MAX_KEYS		1024
#define SMC_TIMEOUT_MSEC	250

struct apple_m1_smc {
	struct device *dev;
	struct apple_rtkit *rtk;
	struct completion cmdcompl;
	unsigned long timeout;
	u64 rxmsg;
	void __iomem *buf;
	struct mutex lock;
	u32 msgid;

	struct gpio_chip gpio;
	u32 gpio_present_mask;
	u32 gpio_bits[MAX_GPIO];
};

struct apple_m1_smc_key_info {
	u8 size;
	u32 type;
	u8 flags;
} __packed;

static void apple_m1_smc_write_buf(void __iomem *buf, const void *mem, size_t size)
{
	u32 __iomem *bufw = buf;
	const u32 *memw = mem;
	u32 tmp;

	while(size >= 4) {
		writel(*(memw ++), bufw);
		bufw ++;
		size -= 4;
	}

	if(size) {
		tmp = 0;
		memcpy(&tmp, memw, size);
		writel(tmp, bufw);
	}
}

static void apple_m1_smc_read_buf(void *mem, void __iomem *buf, size_t size)
{
	u32 __iomem *bufw = buf;
	u32 *memw = mem;
	u32 tmp;

	while(size >= 4) {
		*(memw ++) = readl(bufw);
		bufw ++;
		size -= 4;
	}

	if(size) {
		tmp = readl(bufw);
		memcpy(memw, &tmp, size);
	}
}

static int apple_m1_smc_cmd(struct apple_m1_smc *smc, u8 cmd, u16 hparam, u32 wparam,
			    const void *din, unsigned dilen, void *dout, unsigned dolen, u64 *out)
{
	int ret;
	u64 msg[2], msg0;

	if(dilen > SMC_BUF_SIZE || dolen > SMC_BUF_SIZE)
		return -EFBIG;

	mutex_lock(&smc->lock);
	if(dilen && din)
		apple_m1_smc_write_buf(smc->buf, din, dilen);
	smc->msgid = (smc->msgid + 1) & 15;

	init_completion(&smc->cmdcompl);

	msg0 = cmd | ((u64)hparam << 16) | ((u64)wparam << 32) | (smc->msgid << 12);
	apple_rtkit_send_message(smc->rtk, SMC_ENDPOINT, msg0);

	if(ret >= 0)
		ret = wait_for_completion_timeout(&smc->cmdcompl, smc->timeout) ? 0 : -ETIMEDOUT;

	if(ret >= 0) {
		if(dolen && dout)
			apple_m1_smc_read_buf(dout, smc->buf, dolen);
		if(out)
			*out = smc->rxmsg;
		msg0 = smc->rxmsg;
	}
	mutex_unlock(&smc->lock);

	if(ret < 0) {
		dev_warn(smc->dev, "command [%016llx] failed: %d.\n", msg[0], ret);
		return ret;
	}

	ret = msg0 & 255;
	if(ret) {
		if(cmd != SMC_GET_KEY_BY_INDEX) /* key enumeration would be noisy */
			dev_warn(smc->dev, "command [%016llx] failed: %d.\n", msg[0], ret);
		return -EIO;
	}
	return 0;
}

static int apple_m1_smc_write_key(struct apple_m1_smc *smc, u32 key, const void *data, size_t size)
{
	return apple_m1_smc_cmd(smc, SMC_WRITE_KEY, size, key, data, size, NULL, 0, NULL);
}

static int apple_m1_smc_get_key_info(struct apple_m1_smc *smc, u32 key,
				     struct apple_m1_smc_key_info *ki)
{
	return apple_m1_smc_cmd(smc, SMC_GET_KEY_INFO, 0, key, NULL, 0, ki, sizeof(*ki), NULL);
}

static int apple_m1_smc_read_key_payload(struct apple_m1_smc *smc, u32 key,
					 const void *pld, size_t psize, void *data, size_t size)
{
	u64 out;
	int ret, outlen;

	ret = apple_m1_smc_cmd(smc, pld ? SMC_READ_KEY_PAYLOAD : SMC_READ_KEY, size | (psize << 8),
			       key, pld, psize, (size > 4) ? data : NULL, size, &out);
	if(ret < 0)
		return ret;

	outlen = (out >> 16) & 0xFFFF;
	if(outlen < size) {
		dev_warn(smc->dev, "READ_KEY [%08x, %d] result too big: %d.\n",
			 key, (int)size, outlen);
		return -ENOSPC;
	}

	if(outlen <= 4)
		memcpy(data, (void *)out + 4, size);
	return outlen;
}

static int apple_m1_smc_read_key(struct apple_m1_smc *smc, u32 key, void *data, size_t size)
{
	return apple_m1_smc_read_key_payload(smc, key, NULL, 0, data, size);
}

static int apple_m1_smc_get_key_by_index(struct apple_m1_smc *smc, u32 index, u32 *key)
{
	u64 out;
	int ret;

	ret = apple_m1_smc_cmd(smc, SMC_GET_KEY_BY_INDEX, 0, index, NULL, 0, NULL, 0, &out);
	if(ret < 0)
		return ret;

	if(key)
		*key = swab32(out >> 32);
	return 0;
}

static u64 apple_m1_pack_hex(u32 val, unsigned len)
{
	unsigned i;
	u64 res = 0;
	for(i=0; i<len; i++) {
		res |= (u64)("0123456789abcdef"[val & 15]) << (i * 8);
		val >>= 4;
	}
	return res;
}

static int apple_m1_smc_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	return -EINVAL;
}

static int apple_m1_smc_direction_output(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct apple_m1_smc *smc = gpiochip_get_data(chip);
	u32 key, data;

	if(!(smc->gpio_present_mask & (1 << offset)))
		return -ENODEV;

	key = 0x67500000 | apple_m1_pack_hex(offset, 2); /* gP-- */
	data = (!!value) | smc->gpio_bits[offset];
	return apple_m1_smc_write_key(smc, key, &data, sizeof(data));
}

static int apple_m1_smc_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static int apple_m1_smc_get(struct gpio_chip *chip, unsigned int offset)
{
	struct apple_m1_smc *smc = gpiochip_get_data(chip);
	u32 key, data;
	int ret;

	if(!(smc->gpio_present_mask & (1 << offset)))
		return -ENODEV;

	key = 0x67500000 | apple_m1_pack_hex(offset, 2); /* gP-- */
	ret = apple_m1_smc_read_key(smc, key, &data, sizeof(data));
	if(ret < 0)
		return ret;
	return data & 1;
}

static void apple_m1_smc_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	apple_m1_smc_direction_output(chip, offset, value);
}

static int apple_m1_smc_enumerate(struct apple_m1_smc *smc)
{
	unsigned idx;
	u32 key;
	struct apple_m1_smc_key_info ki;
	int ret;

	for(idx=0; idx<SMC_MAX_KEYS; idx++) {
		ret = apple_m1_smc_get_key_by_index(smc, idx, &key);
		if(ret)
			break;

		ret = apple_m1_smc_get_key_info(smc, key, &ki);
		if(ret)
			continue;
	}

	return 0;
}

static void rtk_got_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_m1_smc *smc = cookie;

	if((message & 0xFF) == SMC_NOTIFICATION) {
		dev_info(smc->dev, "notification: %016llx.\n", message);
		return;
	}

	smc->rxmsg = message;
	complete(&smc->cmdcompl);
}

static int dummy_shmem_verify(void *cookie, dma_addr_t addr, size_t len)
{
        return 0;
}

static struct apple_rtkit_ops rtkit_ops =
{
        .shmem_owner = APPLE_RTKIT_SHMEM_OWNER_RTKIT,
        .shmem_verify = dummy_shmem_verify,
        .recv_message = rtk_got_msg,
};

static int apple_m1_smc_probe(struct platform_device *pdev)
{
	struct apple_m1_smc *smc;
	struct device *dev = &pdev->dev;
        struct resource *res;

	char name[16];
	u64 msg[2];
	int ret, i;

	smc = devm_kzalloc(&pdev->dev, sizeof(*smc), GFP_KERNEL);
	if (!smc)
		return -ENOMEM;
	smc->dev = dev;
	mutex_init(&smc->lock);
	init_completion(&smc->cmdcompl);
	smc->timeout = msecs_to_jiffies(SMC_TIMEOUT_MSEC);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

        res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "coproc");
        if (!res)
                return -EINVAL;

        smc->rtk = apple_rtkit_init(dev, smc, res, "mbox", &rtkit_ops);
	apple_rtkit_boot_wait(smc->rtk);
	apple_rtkit_start_ep(smc->rtk, SMC_ENDPOINT);

	for(i=0; i<MAX_GPIO; i++) {
		snprintf(name, sizeof(name), "gpio-%d", i);
		if(device_property_read_u32(dev, name, &smc->gpio_bits[i]) >= 0)
			smc->gpio_present_mask |= 1 << i;
	}

	ret = apple_m1_smc_cmd(smc, SMC_GET_SRAM_ADDR, 0, 0, NULL, 0, NULL, 0, msg);
	if(ret) {
		dev_err(dev, "failed to start SMC: %d.\n", ret);
		return ret;
	}
	smc->buf = devm_ioremap_np(dev, msg[0], SMC_BUF_SIZE);
	if(!smc->buf) {
		dev_err(dev, "failed to map SMC buffer at 0x%llx.\n", msg[0]);
		return -EINVAL;
	}

	ret = apple_m1_smc_enumerate(smc);
	if(ret < 0)
		return ret;

	smc->gpio.direction_input = apple_m1_smc_direction_input;
	smc->gpio.direction_output = apple_m1_smc_direction_output;
	smc->gpio.get_direction = apple_m1_smc_get_direction;
	smc->gpio.get = apple_m1_smc_get;
	smc->gpio.set = apple_m1_smc_set;

	smc->gpio.ngpio = MAX_GPIO;
	smc->gpio.label = "apple-m1-smc";

	smc->gpio.base = -1;
	smc->gpio.can_sleep = true;
	smc->gpio.parent = &pdev->dev;
	smc->gpio.owner = THIS_MODULE;

	return devm_gpiochip_add_data(&pdev->dev, &smc->gpio, smc);
}

static const struct of_device_id apple_m1_smc_of_match[] = {
	{ .compatible = "apple,smc-m1" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_m1_smc_of_match);

static struct platform_driver apple_m1_smc_platform_driver = {
	.driver = {
		.name = "apple-m1-smc",
		.of_match_table = apple_m1_smc_of_match,
	},
	.probe = apple_m1_smc_probe,
};
module_platform_driver(apple_m1_smc_platform_driver);

MODULE_DESCRIPTION("Apple M1 SMC driver");
MODULE_LICENSE("GPL v2");
