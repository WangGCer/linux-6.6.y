// SPDX-License-Identifier: GPL-2.0
/*
 * rockchip-rng.c True Random Number Generator driver for Rockchip SoCs
 *
 * Copyright (c) 2018, Fuzhou Rockchip Electronics Co., Ltd.
 * Copyright (c) 2022, Aurelien Jarno
 * Authors:
 *  Lin Jinhan <troy.lin@rock-chips.com>
 *  Aurelien Jarno <aurelien@aurel32.net>
 */
#include <linux/clk.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>

#define RK_RNG_AUTOSUSPEND_DELAY	100
#define RK_RNG_MAX_BYTE			32
#define RK_RNG_POLL_PERIOD_US		100
#define RK_RNG_POLL_TIMEOUT_US		10000

/*
 * TRNG collects osc ring output bit every RK_RNG_SAMPLE_CNT time. The value is
 * a tradeoff between speed and quality and has been adjusted to get a quality
 * of ~900 (~90% of FIPS 140-2 successes).
 */
#define RK_RNG_SAMPLE_CNT		1000

/* TRNG registers from RK3568 TRM-Part2, section 5.4.1 */
#define TRNG_RST_CTL			0x0004
#define TRNG_RNG_CTL			0x0400
#define TRNG_RNG_CTL_LEN_64_BIT		(0x00 << 4)
#define TRNG_RNG_CTL_LEN_128_BIT	(0x01 << 4)
#define TRNG_RNG_CTL_LEN_192_BIT	(0x02 << 4)
#define TRNG_RNG_CTL_LEN_256_BIT	(0x03 << 4)
#define TRNG_RNG_CTL_OSC_RING_SPEED_0	(0x00 << 2)
#define TRNG_RNG_CTL_OSC_RING_SPEED_1	(0x01 << 2)
#define TRNG_RNG_CTL_OSC_RING_SPEED_2	(0x02 << 2)
#define TRNG_RNG_CTL_OSC_RING_SPEED_3	(0x03 << 2)
#define TRNG_RNG_CTL_ENABLE		BIT(1)
#define TRNG_RNG_CTL_START		BIT(0)
#define TRNG_RNG_SAMPLE_CNT		0x0404
#define TRNG_RNG_DOUT_0			0x0410
#define TRNG_RNG_DOUT_1			0x0414
#define TRNG_RNG_DOUT_2			0x0418
#define TRNG_RNG_DOUT_3			0x041c
#define TRNG_RNG_DOUT_4			0x0420
#define TRNG_RNG_DOUT_5			0x0424
#define TRNG_RNG_DOUT_6			0x0428
#define TRNG_RNG_DOUT_7			0x042c

struct rk_rng {
	struct hwrng rng;
	void __iomem *base;
	struct reset_control *rst;
	int clk_num;
	struct clk_bulk_data *clk_bulks;
};

/* The mask determine the bits that are updated */
static void rk_rng_write_ctl(struct rk_rng *rng, u32 val, u32 mask)
{
	writel_relaxed((mask << 16) | val, rng->base + TRNG_RNG_CTL);
}

static int rk_rng_init(struct hwrng *rng)
{
	struct rk_rng *rk_rng = container_of(rng, struct rk_rng, rng);
	u32 reg;
	int ret;

	/* start clocks */
	ret = clk_bulk_prepare_enable(rk_rng->clk_num, rk_rng->clk_bulks);
	if (ret < 0) {
		dev_err((struct device *) rk_rng->rng.priv,
			"Failed to enable clks %d\n", ret);
		return ret;
	}

	/* set the sample period */
	writel(RK_RNG_SAMPLE_CNT, rk_rng->base + TRNG_RNG_SAMPLE_CNT);

	/* set osc ring speed and enable it */
	reg = TRNG_RNG_CTL_LEN_256_BIT |
		   TRNG_RNG_CTL_OSC_RING_SPEED_0 |
		   TRNG_RNG_CTL_ENABLE;
	rk_rng_write_ctl(rk_rng, reg, 0xffff);

	return 0;
}

static void rk_rng_cleanup(struct hwrng *rng)
{
	struct rk_rng *rk_rng = container_of(rng, struct rk_rng, rng);
	u32 reg;

	/* stop TRNG */
	reg = 0;
	rk_rng_write_ctl(rk_rng, reg, 0xffff);

	/* stop clocks */
	clk_bulk_disable_unprepare(rk_rng->clk_num, rk_rng->clk_bulks);
}

static int rk_rng_read(struct hwrng *rng, void *buf, size_t max, bool wait)
{
	struct rk_rng *rk_rng = container_of(rng, struct rk_rng, rng);
	u32 reg;
	int ret = 0;
	int i;

	pm_runtime_get_sync((struct device *) rk_rng->rng.priv);

	/* Start collecting random data */
	reg = TRNG_RNG_CTL_START;
	rk_rng_write_ctl(rk_rng, reg, reg);

	ret = readl_poll_timeout(rk_rng->base + TRNG_RNG_CTL, reg,
				 !(reg & TRNG_RNG_CTL_START),
				 RK_RNG_POLL_PERIOD_US,
				 RK_RNG_POLL_TIMEOUT_US);
	if (ret < 0)
		goto out;

	/* Read random data stored in the registers */
	ret = min_t(size_t, max, RK_RNG_MAX_BYTE);
	for (i = 0; i < ret; i += 4) {
		*(u32 *)(buf + i) = readl_relaxed(rk_rng->base + TRNG_RNG_DOUT_0 + i);
	}

out:
	pm_runtime_mark_last_busy((struct device *) rk_rng->rng.priv);
	pm_runtime_put_sync_autosuspend((struct device *) rk_rng->rng.priv);

	return ret;
}

static int rk_rng_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rk_rng *rk_rng;
	int ret;

	rk_rng = devm_kzalloc(dev, sizeof(struct rk_rng), GFP_KERNEL);
	if (!rk_rng)
		return -ENOMEM;

	rk_rng->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rk_rng->base))
		return PTR_ERR(rk_rng->base);

	rk_rng->clk_num = devm_clk_bulk_get_all(dev, &rk_rng->clk_bulks);
	if (rk_rng->clk_num < 0)
		return dev_err_probe(dev, rk_rng->clk_num,
				     "Failed to get clks property\n");

	rk_rng->rst = devm_reset_control_array_get(&pdev->dev, false, false);
	if (IS_ERR(rk_rng->rst))
		return dev_err_probe(dev, PTR_ERR(rk_rng->rst),
				     "Failed to get reset property\n");

	reset_control_assert(rk_rng->rst);
	udelay(2);
	reset_control_deassert(rk_rng->rst);

	platform_set_drvdata(pdev, rk_rng);

	rk_rng->rng.name = dev_driver_string(dev);
#ifndef CONFIG_PM
	rk_rng->rng.init = rk_rng_init;
	rk_rng->rng.cleanup = rk_rng_cleanup;
#endif
	rk_rng->rng.read = rk_rng_read;
	rk_rng->rng.priv = (unsigned long) dev;
	rk_rng->rng.quality = 900;

	pm_runtime_set_autosuspend_delay(dev, RK_RNG_AUTOSUSPEND_DELAY);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);

	ret = devm_hwrng_register(dev, &rk_rng->rng);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to register Rockchip hwrng\n");

	dev_info(&pdev->dev, "Registered Rockchip hwrng\n");

	return 0;
}

static int rk_rng_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);

	return 0;
}

#ifdef CONFIG_PM
static int rk_rng_runtime_suspend(struct device *dev)
{
	struct rk_rng *rk_rng = dev_get_drvdata(dev);

	rk_rng_cleanup(&rk_rng->rng);

	return 0;
}

static int rk_rng_runtime_resume(struct device *dev)
{
	struct rk_rng *rk_rng = dev_get_drvdata(dev);

	return rk_rng_init(&rk_rng->rng);
}
#endif

static const struct dev_pm_ops rk_rng_pm_ops = {
	SET_RUNTIME_PM_OPS(rk_rng_runtime_suspend,
				rk_rng_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static const struct of_device_id rk_rng_dt_match[] = {
	{
		.compatible = "rockchip,rk3568-rng",
	},
	{},
};

MODULE_DEVICE_TABLE(of, rk_rng_dt_match);

static struct platform_driver rk_rng_driver = {
	.driver	= {
		.name	= "rk3568-rng",
		.pm	= &rk_rng_pm_ops,
		.of_match_table = rk_rng_dt_match,
	},
	.probe	= rk_rng_probe,
	.remove = rk_rng_remove,
};

module_platform_driver(rk_rng_driver);

MODULE_DESCRIPTION("Rockchip True Random Number Generator driver");
MODULE_AUTHOR("Lin Jinhan <troy.lin@rock-chips.com>, Aurelien Jarno <aurelien@aurel32.net>");
MODULE_LICENSE("GPL v2");
