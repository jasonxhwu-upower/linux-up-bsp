// SPDX-License-Identifier: GPL-2.0
/*
 *  R-Car Gen3 THS thermal sensor driver
 *  Based on rcar_thermal.c and work from Hien Dang and Khiem Nguyen.
 *
 * Copyright (C) 2016 Renesas Electronics Corporation.
 * Copyright (C) 2016 Sang Engineering
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sys_soc.h>
#include <linux/thermal.h>

#include "thermal_core.h"
#include "thermal_hwmon.h"

/* Register offsets */
#define REG_GEN3_IRQSTR		0x04
#define REG_GEN3_IRQMSK		0x08
#define REG_GEN3_IRQCTL		0x0C
#define REG_GEN3_IRQEN		0x10
#define REG_GEN3_IRQTEMP1	0x14
#define REG_GEN3_IRQTEMP2	0x18
#define REG_GEN3_IRQTEMP3	0x1C
#define REG_GEN3_CTSR		0x20
#define REG_GEN3_THCTR		0x20
#define REG_GEN3_TEMP		0x28
#define REG_GEN3_THCODE1	0x50
#define REG_GEN3_THCODE2	0x54
#define REG_GEN3_THCODE3	0x58

/* FUSE register base and offsets */
#define PTAT_BASE               0xE6198000
#define REG_GEN3_PTAT1          0x5C
#define REG_GEN3_PTAT2          0x60
#define REG_GEN3_PTAT3          0x64
#define REG_GEN3_THSCP          0x68
#define REG_GEN3_MAX_SIZE       (REG_GEN3_THSCP + 0x4)

/* IRQ{STR,MSK,EN} bits */
#define IRQ_TEMP1		BIT(0)
#define IRQ_TEMP2		BIT(1)
#define IRQ_TEMP3		BIT(2)
#define IRQ_TEMPD1		BIT(3)
#define IRQ_TEMPD2		BIT(4)
#define IRQ_TEMPD3		BIT(5)

/* THSCP bit */
#define COR_PARA_VLD            (0x3 << 14)

/* CTSR bits */
#define CTSR_PONM	BIT(8)
#define CTSR_AOUT	BIT(7)
#define CTSR_THBGR	BIT(5)
#define CTSR_VMEN	BIT(4)
#define CTSR_VMST	BIT(1)
#define CTSR_THSST	BIT(0)

/* THCTR bits */
#define THCTR_PONM	BIT(6)
#define THCTR_THSST	BIT(0)

#define CTEMP_MASK	0xFFF

#define MCELSIUS(temp)	((temp) * 1000)
#define GEN3_FUSE_MASK	0xFFF

#define TSC_MAX_NUM	3

/* ECM register base and offsets */
#define ECM_TSC         16
#define ECM_BASE        0xE6250000
#define CTLR(m)         (0x0000 + (4 * (m)))
#define STSR(m)         (0x0100 + (4 * (m)))
#define TGTR(m)         (0x0200 + (4 * (m)))
#define ECMWPCNTR       (0x0A00)
#define ECM_MAX_SIZE    (ECMWPCNTR + 0x04)
#define ECM_SET         (0xFFF << 14)

/* default THCODE values if FUSEs are missing */
static int thcodes[TSC_MAX_NUM][3] = {
	{ 3397, 2800, 2221 },
	{ 3393, 2795, 2216 },
	{ 3389, 2805, 2237 },
};

/* Structure for thermal temperature calculation */
struct equation_coefs {
	int a1;
	int b1;
	int a2;
	int b2;
};

struct rcar_gen3_thermal_tsc {
	void __iomem *base;
	struct thermal_zone_device *zone;
	struct equation_coefs coef;
	int tj_t;
	unsigned int id; /* thermal channel id */
	bool irq_cap;
};

struct rcar_gen3_thermal_priv {
	struct rcar_gen3_thermal_tsc *tscs[TSC_MAX_NUM];
	unsigned int num_tscs;
	void (*thermal_init)(struct rcar_gen3_thermal_tsc *tsc);
};

static inline u32 rcar_gen3_thermal_read(struct rcar_gen3_thermal_tsc *tsc,
					 u32 reg)
{
	return ioread32(tsc->base + reg);
}

static inline void rcar_gen3_thermal_write(struct rcar_gen3_thermal_tsc *tsc,
					   u32 reg, u32 data)
{
	iowrite32(data, tsc->base + reg);
}

/*
 * Linear approximation for temperature
 *
 * [reg] = [temp] * a + b => [temp] = ([reg] - b) / a
 *
 * The constants a and b are calculated using two triplets of int values PTAT
 * and THCODE. PTAT and THCODE can either be read from hardware or use hard
 * coded values from driver. The formula to calculate a and b are taken from
 * BSP and sparsely documented and understood.
 *
 * Examining the linear formula and the formula used to calculate constants a
 * and b while knowing that the span for PTAT and THCODE values are between
 * 0x000 and 0xfff the largest integer possible is 0xfff * 0xfff == 0xffe001.
 * Integer also needs to be signed so that leaves 7 bits for binary
 * fixed point scaling.
 */

#define FIXPT_SHIFT 7
#define FIXPT_INT(_x) ((_x) << FIXPT_SHIFT)
#define INT_FIXPT(_x) ((_x) >> FIXPT_SHIFT)
#define FIXPT_DIV(_a, _b) DIV_ROUND_CLOSEST(((_a) << FIXPT_SHIFT), (_b))
#define FIXPT_TO_MCELSIUS(_x) ((_x) * 1000 >> FIXPT_SHIFT)

#define RCAR3_THERMAL_GRAN 500 /* mili Celsius */

/* no idea where these constants come from */
#define TJ_3 -41

static inline void ecm_write(u32 value, u32 base, u32 reg)
{
	void __iomem *ecm_base;

	ecm_base = ioremap_cache(base, ECM_MAX_SIZE);

	iowrite32(value, ecm_base + reg);

	iounmap(ecm_base);
}

static void init_ecm_registers(void)
{
	ecm_write(0xACCE0001, ECM_BASE, ECMWPCNTR);
	usleep_range(1000, 2000);
	ecm_write(ECM_SET, ECM_BASE, CTLR(ECM_TSC));
	ecm_write(ECM_SET, ECM_BASE, STSR(ECM_TSC));
	ecm_write(ECM_SET, ECM_BASE, TGTR(ECM_TSC));
}

static const struct soc_device_attribute r8a779f0[] = {
	{ .soc_id = "r8a779f0"},
	{ /* sentinel */ }
};

static void rcar_gen3_thermal_calc_coefs(struct rcar_gen3_thermal_tsc *tsc,
					 int *ptat, const int *thcode,
					 int ths_tj_1)
{
	/* TODO: Find documentation and document constant calculation formula */

	/*
	 * Division is not scaled in BSP and if scaled it might overflow
	 * the dividend (4095 * 4095 << 14 > INT_MAX) so keep it unscaled
	 */
	tsc->tj_t = (FIXPT_INT((ptat[1] - ptat[2]) * (ths_tj_1 - TJ_3))
		     / (ptat[0] - ptat[2])) + FIXPT_INT(TJ_3);

	tsc->coef.a1 = FIXPT_DIV(FIXPT_INT(thcode[1] - thcode[2]),
				 tsc->tj_t - FIXPT_INT(TJ_3));
	tsc->coef.b1 = FIXPT_INT(thcode[2]) - tsc->coef.a1 * TJ_3;

	tsc->coef.a2 = FIXPT_DIV(FIXPT_INT(thcode[1] - thcode[0]),
				 tsc->tj_t - FIXPT_INT(ths_tj_1));
	tsc->coef.b2 = FIXPT_INT(thcode[0]) - tsc->coef.a2 * ths_tj_1;
}

static int rcar_gen3_thermal_round(int temp)
{
	int result, round_offs;

	round_offs = temp >= 0 ? RCAR3_THERMAL_GRAN / 2 :
		-RCAR3_THERMAL_GRAN / 2;
	result = (temp + round_offs) / RCAR3_THERMAL_GRAN;
	return result * RCAR3_THERMAL_GRAN;
}

static int rcar_gen3_thermal_get_temp(void *devdata, int *temp)
{
	struct rcar_gen3_thermal_tsc *tsc = devdata;
	int mcelsius, val;
	int reg;

	/* Read register and convert to mili Celsius */
	reg = rcar_gen3_thermal_read(tsc, REG_GEN3_TEMP) & CTEMP_MASK;

	if (reg <= thcodes[tsc->id][1])
		val = FIXPT_DIV(FIXPT_INT(reg) - tsc->coef.b1,
				tsc->coef.a1);
	else
		val = FIXPT_DIV(FIXPT_INT(reg) - tsc->coef.b2,
				tsc->coef.a2);
	mcelsius = FIXPT_TO_MCELSIUS(val);

	/* Guaranteed operating range is -40C to 125C. */

	/* Round value to device granularity setting */
	*temp = rcar_gen3_thermal_round(mcelsius);

	return 0;
}

static int rcar_gen3_thermal_mcelsius_to_temp(struct rcar_gen3_thermal_tsc *tsc,
					      int mcelsius)
{
	int celsius, val;

	celsius = DIV_ROUND_CLOSEST(mcelsius, 1000);
	if (celsius <= INT_FIXPT(tsc->tj_t))
		val = celsius * tsc->coef.a1 + tsc->coef.b1;
	else
		val = celsius * tsc->coef.a2 + tsc->coef.b2;

	return INT_FIXPT(val);
}

static int rcar_gen3_thermal_set_trips(void *devdata, int low, int high)
{
	struct rcar_gen3_thermal_tsc *tsc = devdata;
	u32 irqmsk = 0;

	if (low != -INT_MAX) {
		irqmsk |= IRQ_TEMPD1;
		rcar_gen3_thermal_write(tsc, REG_GEN3_IRQTEMP1,
					rcar_gen3_thermal_mcelsius_to_temp(tsc, low));
	}

	if (high != INT_MAX) {
		irqmsk |= IRQ_TEMP2;
		rcar_gen3_thermal_write(tsc, REG_GEN3_IRQTEMP2,
					rcar_gen3_thermal_mcelsius_to_temp(tsc, high));
	}

	rcar_gen3_thermal_write(tsc, REG_GEN3_IRQMSK, irqmsk);

	return 0;
}

static struct thermal_zone_of_device_ops rcar_gen3_tz_of_ops = {
	.get_temp	= rcar_gen3_thermal_get_temp,
	.set_trips	= rcar_gen3_thermal_set_trips,
};

static irqreturn_t rcar_gen3_thermal_irq(int irq, void *data)
{
	struct rcar_gen3_thermal_priv *priv = data;
	unsigned int i;
	u32 status;

	for (i = 0; i < priv->num_tscs; i++) {
		status = rcar_gen3_thermal_read(priv->tscs[i], REG_GEN3_IRQSTR);
		rcar_gen3_thermal_write(priv->tscs[i], REG_GEN3_IRQSTR, 0);
		if (soc_device_match(r8a779f0))
			ecm_write(ECM_SET, ECM_BASE, STSR(ECM_TSC)); /* Clear the ECM status */
		if (status)
			thermal_zone_device_update(priv->tscs[i]->zone,
						   THERMAL_EVENT_UNSPECIFIED);
	}

	return IRQ_HANDLED;
}

static const struct soc_device_attribute r8a7795es1[] = {
	{ .soc_id = "r8a7795", .revision = "ES1.*" },
	{ /* sentinel */ }
};

static void rcar_gen3_thermal_init_r8a7795es1(struct rcar_gen3_thermal_tsc *tsc)
{
	rcar_gen3_thermal_write(tsc, REG_GEN3_CTSR,  CTSR_THBGR);
	rcar_gen3_thermal_write(tsc, REG_GEN3_CTSR,  0x0);

	usleep_range(1000, 2000);

	rcar_gen3_thermal_write(tsc, REG_GEN3_CTSR, CTSR_PONM);

	rcar_gen3_thermal_write(tsc, REG_GEN3_IRQCTL, 0x3F);
	rcar_gen3_thermal_write(tsc, REG_GEN3_IRQMSK, 0);
	if (tsc->zone->ops->set_trips)
		rcar_gen3_thermal_write(tsc, REG_GEN3_IRQEN,
					IRQ_TEMPD1 | IRQ_TEMP2);

	rcar_gen3_thermal_write(tsc, REG_GEN3_CTSR,
				CTSR_PONM | CTSR_AOUT | CTSR_THBGR | CTSR_VMEN);

	usleep_range(100, 200);

	rcar_gen3_thermal_write(tsc, REG_GEN3_CTSR,
				CTSR_PONM | CTSR_AOUT | CTSR_THBGR | CTSR_VMEN |
				CTSR_VMST | CTSR_THSST);

	usleep_range(1000, 2000);
}

static void rcar_gen3_thermal_init(struct rcar_gen3_thermal_tsc *tsc)
{
	u32 reg_val;

	reg_val = rcar_gen3_thermal_read(tsc, REG_GEN3_THCTR);
	reg_val &= ~THCTR_PONM;
	rcar_gen3_thermal_write(tsc, REG_GEN3_THCTR, reg_val);

	usleep_range(1000, 2000);

	rcar_gen3_thermal_write(tsc, REG_GEN3_IRQCTL, 0);
	rcar_gen3_thermal_write(tsc, REG_GEN3_IRQMSK, 0);
	if (tsc->zone->ops->set_trips)
		rcar_gen3_thermal_write(tsc, REG_GEN3_IRQEN,
					IRQ_TEMPD1 | IRQ_TEMP2);

	reg_val = rcar_gen3_thermal_read(tsc, REG_GEN3_THCTR);
	reg_val |= THCTR_THSST;
	rcar_gen3_thermal_write(tsc, REG_GEN3_THCTR, reg_val);

	usleep_range(1000, 2000);
}

static const int rcar_gen3_ths_tj_1 = 126;
static const int rcar_gen3_ths_tj_1_m3_w = 116;
static const struct of_device_id rcar_gen3_thermal_dt_ids[] = {
	{
		.compatible = "renesas,r8a774a1-thermal",
		.data = &rcar_gen3_ths_tj_1_m3_w,
	},
	{
		.compatible = "renesas,r8a774b1-thermal",
		.data = &rcar_gen3_ths_tj_1,
	},
	{
		.compatible = "renesas,r8a774e1-thermal",
		.data = &rcar_gen3_ths_tj_1,
	},
	{
		.compatible = "renesas,r8a7795-thermal",
		.data = &rcar_gen3_ths_tj_1,
	},
	{
		.compatible = "renesas,r8a7796-thermal",
		.data = &rcar_gen3_ths_tj_1_m3_w,
	},
	{
		.compatible = "renesas,r8a77961-thermal",
		.data = &rcar_gen3_ths_tj_1_m3_w,
	},
	{
		.compatible = "renesas,r8a77965-thermal",
		.data = &rcar_gen3_ths_tj_1,
	},
	{
		.compatible = "renesas,r8a77980-thermal",
		.data = &rcar_gen3_ths_tj_1,
	},
	{
		.compatible = "renesas,r8a779f0-thermal",
		.data = &rcar_gen3_ths_tj_1,
	},
	{},
};
MODULE_DEVICE_TABLE(of, rcar_gen3_thermal_dt_ids);

static int rcar_gen3_thermal_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	pm_runtime_put(dev);
	pm_runtime_disable(dev);

	return 0;
}

static void rcar_gen3_hwmon_action(void *data)
{
	struct thermal_zone_device *zone = data;

	thermal_remove_hwmon_sysfs(zone);
}

static int rcar_gen3_thermal_request_irqs(struct rcar_gen3_thermal_priv *priv,
					  struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int i, irq_num;
	char *irqname;
	int ret, irq;

	irq_num = 2;
	if (soc_device_match(r8a779f0)) {
		init_ecm_registers();
		irq_num = 1;
	}

	for (i = 0; i < irq_num; i++) {
		irq = platform_get_irq_optional(pdev, i);
		if (irq < 0)
			return irq;

		irqname = devm_kasprintf(dev, GFP_KERNEL, "%s:ch%d",
					 dev_name(dev), i);
		if (!irqname)
			return -ENOMEM;

		ret = devm_request_threaded_irq(dev, irq, NULL,
						rcar_gen3_thermal_irq,
						IRQF_ONESHOT, irqname, priv);
		if (ret)
			return ret;
	}

	return 0;
}

static int rcar_gen3_thermal_probe(struct platform_device *pdev)
{
	struct rcar_gen3_thermal_priv *priv;
	struct device *dev = &pdev->dev;
	const int *rcar_gen3_ths_tj_1_const = of_device_get_match_data(dev);
	struct resource *res;
	struct thermal_zone_device *zone;
	unsigned int i;
	int ret;
	void __iomem *ptat_base;
	unsigned int cor_para_value;
	struct device_node *tz_nd;

	/* default values if FUSEs are missing */
	int ptat[3] = { 2631, 1509, 435 };

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->thermal_init = rcar_gen3_thermal_init;
	if (soc_device_match(r8a7795es1))
		priv->thermal_init = rcar_gen3_thermal_init_r8a7795es1;

	platform_set_drvdata(pdev, priv);

	if (rcar_gen3_thermal_request_irqs(priv, pdev))
		rcar_gen3_tz_of_ops.set_trips = NULL;

	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	/* Use FUSE default values if they are missing.
	 * If not, fetch them from registers.
	 */
	ptat_base = ioremap(PTAT_BASE, REG_GEN3_MAX_SIZE);
	if (!ptat_base) {
		dev_err(dev, "Cannot map FUSE register\n");
		return -ENOMEM;
	}

	cor_para_value = ioread32(ptat_base + REG_GEN3_THSCP) & COR_PARA_VLD;

	if (cor_para_value != COR_PARA_VLD) {
		dev_info(dev, "is using pseudo fixed FUSE values\n");
	} else {
		dev_info(dev, "is using FUSE values\n");
		ptat[0] = ioread32(ptat_base + REG_GEN3_PTAT1) & GEN3_FUSE_MASK;
		ptat[1] = ioread32(ptat_base + REG_GEN3_PTAT2) & GEN3_FUSE_MASK;
		ptat[2] = ioread32(ptat_base + REG_GEN3_PTAT3) & GEN3_FUSE_MASK;
	}

	iounmap(ptat_base);

	for (i = 0; i < TSC_MAX_NUM; i++) {
		struct rcar_gen3_thermal_tsc *tsc;

		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res)
			break;

		tsc = devm_kzalloc(dev, sizeof(*tsc), GFP_KERNEL);
		if (!tsc) {
			ret = -ENOMEM;
			goto error_unregister;
		}

		tsc->base = devm_ioremap_resource(dev, res);
		if (IS_ERR(tsc->base)) {
			ret = PTR_ERR(tsc->base);
			goto error_unregister;
		}
		tsc->id = i;

		priv->tscs[i] = tsc;

		if (cor_para_value == COR_PARA_VLD) {
			thcodes[i][0] = GEN3_FUSE_MASK &
				rcar_gen3_thermal_read(tsc, REG_GEN3_THCODE1);
			thcodes[i][1] = GEN3_FUSE_MASK &
				rcar_gen3_thermal_read(tsc, REG_GEN3_THCODE2);
			thcodes[i][2] = GEN3_FUSE_MASK &
				rcar_gen3_thermal_read(tsc, REG_GEN3_THCODE3);
		}

		rcar_gen3_thermal_calc_coefs(tsc, ptat, thcodes[i],
					     *rcar_gen3_ths_tj_1_const);

		for_each_node_with_property(tz_nd, "polling-delay") {
			u32 zone_id, idle;

			if (of_parse_phandle(tz_nd, "thermal-sensors", 0)) {
				of_property_read_u32_index(tz_nd,
							   "thermal-sensors",
							   1, &zone_id);
				if (zone_id == i) {
					of_property_read_u32(tz_nd,
							     "polling-delay",
							     &idle);
					tsc->irq_cap = idle ? 0 : 1;
				}
			}
		}

		zone = devm_thermal_zone_of_sensor_register(dev, i, tsc,
							    &rcar_gen3_tz_of_ops);
		if (IS_ERR(zone)) {
			dev_err(dev, "Can't register thermal zone\n");
			ret = PTR_ERR(zone);
			goto error_unregister;
		}
		tsc->zone = zone;

		priv->thermal_init(tsc);

		tsc->zone->tzp->no_hwmon = false;
		ret = thermal_add_hwmon_sysfs(tsc->zone);
		if (ret)
			goto error_unregister;

		ret = devm_add_action_or_reset(dev, rcar_gen3_hwmon_action, zone);
		if (ret)
			goto error_unregister;

		ret = of_thermal_get_ntrips(tsc->zone);
		if (ret < 0)
			goto error_unregister;

		dev_info(dev, "TSC%u: Loaded %d trip points\n", i, ret);
	}

	priv->num_tscs = i;

	if (!priv->num_tscs) {
		ret = -ENODEV;
		goto error_unregister;
	}

	return 0;

error_unregister:
	rcar_gen3_thermal_remove(pdev);

	return ret;
}

static int __maybe_unused rcar_gen3_thermal_resume(struct device *dev)
{
	struct rcar_gen3_thermal_priv *priv = dev_get_drvdata(dev);
	unsigned int i;

	for (i = 0; i < priv->num_tscs; i++) {
		struct rcar_gen3_thermal_tsc *tsc = priv->tscs[i];
		struct thermal_zone_device *zone = tsc->zone;

		priv->thermal_init(tsc);
		if (zone->ops->set_trips)
			rcar_gen3_thermal_set_trips(tsc, zone->prev_low_trip,
						    zone->prev_high_trip);
	}

	return 0;
}

static SIMPLE_DEV_PM_OPS(rcar_gen3_thermal_pm_ops, NULL,
			 rcar_gen3_thermal_resume);

static struct platform_driver rcar_gen3_thermal_driver = {
	.driver	= {
		.name	= "rcar_gen3_thermal",
		.pm = &rcar_gen3_thermal_pm_ops,
		.of_match_table = rcar_gen3_thermal_dt_ids,
	},
	.probe		= rcar_gen3_thermal_probe,
	.remove		= rcar_gen3_thermal_remove,
};
module_platform_driver(rcar_gen3_thermal_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("R-Car Gen3 THS thermal sensor driver");
MODULE_AUTHOR("Wolfram Sang <wsa+renesas@sang-engineering.com>");
