// SPDX-License-Identifier: GPL-2.0
/*
 * R-Car SYSC Power management support
 *
 * Copyright (C) 2014  Magnus Damm
 * Copyright (C) 2015-2017 Glider bvba
 * Copyright (C) 2021 Renesas Electronics Corporation
 */

#include <dt-bindings/power/r8a7795-sysc.h>
#include <dt-bindings/power/r8a7796-sysc.h>
#include <dt-bindings/power/r8a77965-sysc.h>
#include <dt-bindings/power/r8a77980-sysc.h>
#include <dt-bindings/power/r8a779f0-sysc.h>
#include <linux/clk/renesas.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/of_address.h>
#include <linux/pm_domain.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/io.h>
#include <linux/soc/renesas/rcar-sysc.h>
#include <linux/sys_soc.h>
#include <linux/syscore_ops.h>

#include "rcar-sysc.h"

/* SYSC Common */
#define SYSCSR			0x00	/* SYSC Status Register */
#define SYSCISR			0x04	/* Interrupt Status Register */
#define SYSCISCR		0x08	/* Interrupt Status Clear Register */
#define SYSCIER			0x0c	/* Interrupt Enable Register */
#define SYSCIMR			0x10	/* Interrupt Mask Register */

/* SYSC Status Register */
#define SYSCSR_PONENB		1	/* Ready for power resume requests */
#define SYSCSR_POFFENB		0	/* Ready for power shutoff requests */

/*
 * Power Control Register Offsets inside the register block for each domain
 * Note: The "CR" registers for ARM cores exist on H1 only
 *	 Use WFI to power off, CPG/APMU to resume ARM cores on R-Car Gen2
 *	 Use PSCI on R-Car Gen3
 */
#define PWRSR_OFFS		0x00	/* Power Status Register */
#define PWROFFCR_OFFS		0x04	/* Power Shutoff Control Register */
#define PWROFFSR_OFFS		0x08	/* Power Shutoff Status Register */
#define PWRONCR_OFFS		0x0c	/* Power Resume Control Register */
#define PWRONSR_OFFS		0x10	/* Power Resume Status Register */
#define PWRER_OFFS		0x14	/* Power Shutoff/Resume Error */


#define SYSCSR_RETRIES		1000
#define SYSCSR_DELAY_US		10

#define PWRER_RETRIES		1000
#define PWRER_DELAY_US		10

#define SYSCISR_RETRIES		1000
#define SYSCISR_DELAY_US	10

#define RCAR_PD_ALWAYS_ON	32	/* Always-on power area */

/* Number of areas need to fixup data when enable PDMODE */
#define NUM_FIXUP_AREAS		14

/* Module Stop Control/Status Register */
#define MSTPSR5_ADDR           0xE615003C
#define MSTPSR8_ADDR           0xE61509A0
#define SMSTPCR5_ADDR          0xE6150144
#define SMSTPCR8_ADDR          0xE6150990

/* Mask for IMP clock in Module Stop Control/Status Register 8 */
#define IMPx8_MASK                     0xff000000

/* Mask for IMP clock in Module Stop Control/Status Register 5 */
#define IMPx5_MASK                     0xbf200001

struct rcar_sysc_ch {
	u16 chan_offs;
	u8 chan_bit;
	u8 isr_bit;
};

static
const struct soc_device_attribute rcar_sysc_quirks_match[] __initconst = {
	{
		.soc_id = "r8a7795", .revision = "ES2.0",
		.data = (void *)(BIT(R8A7795_PD_A3VP) | BIT(R8A7795_PD_CR7)
			| BIT(R8A7795_PD_A3VC) | BIT(R8A7795_PD_A2VC0)
			| BIT(R8A7795_PD_A2VC1) | BIT(R8A7795_PD_A3IR)
			| BIT(R8A7795_PD_3DG_A) | BIT(R8A7795_PD_3DG_B)
			| BIT(R8A7795_PD_3DG_C) | BIT(R8A7795_PD_3DG_D)
			| BIT(R8A7795_PD_3DG_E)),
	},
	{
		.soc_id = "r8a7795", .revision = "ES1.*",
		.data = (void *)(BIT(R8A7795_PD_A3VP) | BIT(R8A7795_PD_CR7)
			| BIT(R8A7795_PD_A3VC) | BIT(R8A7795_PD_A2VC0)
			| BIT(R8A7795_PD_A2VC1) | BIT(R8A7795_PD_A3IR)
			| BIT(R8A7795_PD_3DG_A) | BIT(R8A7795_PD_3DG_B)
			| BIT(R8A7795_PD_3DG_C) | BIT(R8A7795_PD_3DG_D)
			| BIT(R8A7795_PD_3DG_E)),

	},
	{
		.soc_id = "r8a7796", .revision = "ES1.*",
		.data = (void *)(BIT(R8A7796_PD_CR7) | BIT(R8A7796_PD_A3VC)
			| BIT(R8A7796_PD_A2VC0) | BIT(R8A7796_PD_A2VC1)
			| BIT(R8A7796_PD_A3IR) | BIT(R8A7796_PD_3DG_A)
			| BIT(R8A7796_PD_3DG_B)),
	},
	{
		.soc_id = "r8a77980",
		.data = (void *)(BIT(R8A77980_PD_CR7)),
	},
	{ /* sentinel */ }
};

/* Fixups for R-Car V3H revision */
static const struct soc_device_attribute r8a77980[] __initconst = {
		{ .soc_id = "r8a77980"},
		{ /* sentinel */ }
};

static struct
rcar_sysc_area r8a77980_fixup_areas[3][NUM_FIXUP_AREAS] __initdata = {
{
	/* Fix-up area for PDMODE = 1 */
	{ "a2ir0",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2ir1",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2ir2",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2ir3",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2ir4",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2ir5",	0x400, 5, R8A77980_PD_A2IR5,	R8A77980_PD_A3IR },
	{ "a2sc0",	0x400, 6, R8A77980_PD_A2SC0,	R8A77980_PD_A3IR },
	{ "a2sc1",	0x400, 6, R8A77980_PD_A2SC0,	R8A77980_PD_A3IR },
	{ "a2sc2",	0x400, 6, R8A77980_PD_A2SC0,	R8A77980_PD_A3IR },
	{ "a2sc3",	0x400, 6, R8A77980_PD_A2SC0,	R8A77980_PD_A3IR },
	{ "a2sc4",	0x400, 6, R8A77980_PD_A2SC0,	R8A77980_PD_A3IR },
	{ "a2dp0",	0x400, 11, R8A77980_PD_A2DP0,	R8A77980_PD_A3IR },
	{ "a2dp1",	0x400, 11, R8A77980_PD_A2DP0,	R8A77980_PD_A3IR },
	{ "a2cn",	0x400, 13, R8A77980_PD_A2CN,	R8A77980_PD_A3IR },
},
{
    /* No handle for PDMODE = 2 */
},
{
	/* Fix-up area for PDMODE = 3 */
	{ "a2ir0",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2ir1",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2ir2",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2ir3",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2ir4",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2ir5",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2sc0",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2sc1",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2sc2",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2sc3",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2sc4",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2dp0",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2dp1",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
	{ "a2cn",	0x400, 0, R8A77980_PD_A2IR0,	R8A77980_PD_A3IR },
}

};

static u32 rcar_sysc_quirks;
static bool has_clk_crl;

static void __iomem *rcar_sysc_base;
static DEFINE_SPINLOCK(rcar_sysc_lock); /* SMP CPUs + I/O devices */
static u32 rcar_sysc_extmask_offs, rcar_sysc_extmask_val;

static const char *to_pd_name(const struct rcar_sysc_ch *sysc_ch);

static int rcar_sysc_pwr_on_off(const struct rcar_sysc_ch *sysc_ch, bool on)
{
	unsigned int sr_bit, reg_offs;
	int k;

	if (on) {
		sr_bit = SYSCSR_PONENB;
		reg_offs = PWRONCR_OFFS;
	} else {
		sr_bit = SYSCSR_POFFENB;
		reg_offs = PWROFFCR_OFFS;
	}

	/* Wait until SYSC is ready to accept a power request */
	for (k = 0; k < SYSCSR_RETRIES; k++) {
		if (ioread32(rcar_sysc_base + SYSCSR) & BIT(sr_bit))
			break;
		udelay(SYSCSR_DELAY_US);
	}

	if (k == SYSCSR_RETRIES)
		return -EAGAIN;

	/* Start W/A for A3VP, A3VC, and A3IR domains */
	if (!on && (!strcmp("a3vp", to_pd_name(sysc_ch)) ||
		    !strcmp("a3ir", to_pd_name(sysc_ch)) ||
		    !strcmp("a3vc", to_pd_name(sysc_ch))))
		udelay(1);

	/* Submit power shutoff or power resume request */
	iowrite32(BIT(sysc_ch->chan_bit),
		  rcar_sysc_base + sysc_ch->chan_offs + reg_offs);

	return 0;
}

static int rcar_sysc_power(const struct rcar_sysc_ch *sysc_ch, bool on)
{
	unsigned int isr_mask = BIT(sysc_ch->isr_bit);
	unsigned int chan_mask = BIT(sysc_ch->chan_bit);
	unsigned int status;
	unsigned long flags;
	int ret = 0;
	int k;

	spin_lock_irqsave(&rcar_sysc_lock, flags);

	/*
	 * Mask external power requests for CPU or 3DG domains
	 */
	if (rcar_sysc_extmask_val) {
		iowrite32(rcar_sysc_extmask_val,
			  rcar_sysc_base + rcar_sysc_extmask_offs);
	}

	/*
	 * The interrupt source needs to be enabled, but masked, to prevent the
	 * CPU from receiving it.
	 */
	iowrite32(ioread32(rcar_sysc_base + SYSCIMR) | isr_mask,
		  rcar_sysc_base + SYSCIMR);
	iowrite32(ioread32(rcar_sysc_base + SYSCIER) | isr_mask,
		  rcar_sysc_base + SYSCIER);

	iowrite32(isr_mask, rcar_sysc_base + SYSCISCR);

	/* Submit power shutoff or resume request until it was accepted */
	for (k = 0; k < PWRER_RETRIES; k++) {
		ret = rcar_sysc_pwr_on_off(sysc_ch, on);
		if (ret)
			goto out;

		status = ioread32(rcar_sysc_base +
				  sysc_ch->chan_offs + PWRER_OFFS);
		if (!(status & chan_mask))
			break;

		udelay(PWRER_DELAY_US);
	}

	if (k == PWRER_RETRIES) {
		ret = -EIO;
		goto out;
	}

	/* Wait until the power shutoff or resume request has completed * */
	for (k = 0; k < SYSCISR_RETRIES; k++) {
		if (ioread32(rcar_sysc_base + SYSCISR) & isr_mask)
			break;
		udelay(SYSCISR_DELAY_US);
	}

	if (k == SYSCISR_RETRIES)
		ret = -EIO;

	iowrite32(isr_mask, rcar_sysc_base + SYSCISCR);

 out:
	if (rcar_sysc_extmask_val)
		iowrite32(0, rcar_sysc_base + rcar_sysc_extmask_offs);

	spin_unlock_irqrestore(&rcar_sysc_lock, flags);

	pr_debug("sysc power %s domain %d: %08x -> %d\n", on ? "on" : "off",
		 sysc_ch->isr_bit, ioread32(rcar_sysc_base + SYSCISR), ret);
	return ret;
}

static bool rcar_sysc_power_is_off(const struct rcar_sysc_ch *sysc_ch)
{
	unsigned int st;

	st = ioread32(rcar_sysc_base + sysc_ch->chan_offs + PWRSR_OFFS);
	if (st & BIT(sysc_ch->chan_bit))
		return true;

	return false;
}

struct rcar_sysc_pd {
	struct generic_pm_domain genpd;
	struct rcar_sysc_ch ch;
	unsigned int flags;
	char name[];
};

static inline struct rcar_sysc_pd *to_rcar_pd(struct generic_pm_domain *d)
{
	return container_of(d, struct rcar_sysc_pd, genpd);
}

static inline const char *to_pd_name(const struct rcar_sysc_ch *sysc_ch)
{
	return container_of(sysc_ch, struct rcar_sysc_pd, ch)->genpd.name;
}

/* On V3H, necessary to enable/disable IMP clock before A3IR on/off */
static int  rcar_sysc_a3ir_clk_ctrl(bool clk_en)
{
		void __iomem *mstpsr5, *mstpsr8;
		void __iomem *smstpcr5, *smstpcr8;
		u32 val, timeout = 0;

		smstpcr5 = ioremap(SMSTPCR5_ADDR, 0x04);
		smstpcr8 = ioremap(SMSTPCR8_ADDR, 0x04);
		mstpsr5 = ioremap(MSTPSR5_ADDR, 0x04);
		mstpsr8 = ioremap(MSTPSR8_ADDR, 0x04);

		if (clk_en) {
			val = readl(smstpcr5) & ~IMPx5_MASK;
			writel(val, smstpcr5);
			val = readl(smstpcr8) & ~IMPx8_MASK;
			writel(val, smstpcr8);

			while ((readl(mstpsr5) & IMPx5_MASK) |
				   (readl(mstpsr8) & IMPx8_MASK)) {
				udelay(1);
				timeout++;

				if (timeout > 100)
					break;
			}
		} else {
			val = readl(smstpcr5) | IMPx5_MASK;
			writel(val, smstpcr5);
			val = readl(smstpcr8) | IMPx8_MASK;
			writel(val, smstpcr8);

			while (!((readl(mstpsr5) & IMPx5_MASK) &
					(readl(mstpsr8) & IMPx8_MASK))) {
				udelay(1);
				timeout++;

				if (timeout > 100)
					break;
				}
	}

	if (timeout > 100) {
		pr_debug("%s : Fail in %s IMP clock\n", __func__,
			 clk_en ? "enable" : "disable");
		return -EBUSY;
	}

	iounmap(smstpcr5);
	iounmap(smstpcr8);
	iounmap(mstpsr5);
	iounmap(mstpsr8);

	return 0;
}

static int rcar_sysc_pd_power_off(struct generic_pm_domain *genpd)
{
	struct rcar_sysc_pd *pd = to_rcar_pd(genpd);

	if (rcar_sysc_power_is_off(&pd->ch))
		return 0;

	/* Disable IMP clock before power off A3IR */
	if (has_clk_crl && (!strcmp("a3ir", genpd->name)))
		rcar_sysc_a3ir_clk_ctrl(false);

	pr_debug("%s: %s\n", __func__, genpd->name);
	return rcar_sysc_power(&pd->ch, false);
}

static int rcar_sysc_pd_power_on(struct generic_pm_domain *genpd)
{
	struct rcar_sysc_pd *pd = to_rcar_pd(genpd);

	if (!rcar_sysc_power_is_off(&pd->ch))
		return 0;

	/* Enable IMP clock before power on A3IR */
	if (has_clk_crl && (!strcmp("a3ir", genpd->name)))
		rcar_sysc_a3ir_clk_ctrl(true);

	pr_debug("%s: %s\n", __func__, genpd->name);
	return rcar_sysc_power(&pd->ch, true);
}

static bool has_cpg_mstp;

static int __init rcar_sysc_pd_setup(struct rcar_sysc_pd *pd)
{
	struct generic_pm_domain *genpd = &pd->genpd;
	const char *name = pd->genpd.name;
	int error;

	if (pd->flags & PD_CPU) {
		/*
		 * This domain contains a CPU core and therefore it should
		 * only be turned off if the CPU is not in use.
		 */
		pr_debug("PM domain %s contains %s\n", name, "CPU");
		genpd->flags |= GENPD_FLAG_ALWAYS_ON;
	} else if (pd->flags & PD_SCU) {
		/*
		 * This domain contains an SCU and cache-controller, and
		 * therefore it should only be turned off if the CPU cores are
		 * not in use.
		 */
		pr_debug("PM domain %s contains %s\n", name, "SCU");
		genpd->flags |= GENPD_FLAG_ALWAYS_ON;
	} else if (pd->flags & PD_NO_CR) {
		/*
		 * This domain cannot be turned off.
		 */
		genpd->flags |= GENPD_FLAG_ALWAYS_ON;
	}

	if (!(pd->flags & (PD_CPU | PD_SCU))) {
		/* Enable Clock Domain for I/O devices */
		genpd->flags |= GENPD_FLAG_PM_CLK | GENPD_FLAG_ACTIVE_WAKEUP;
		if (has_cpg_mstp) {
			genpd->attach_dev = cpg_mstp_attach_dev;
			genpd->detach_dev = cpg_mstp_detach_dev;
		} else {
			genpd->attach_dev = cpg_mssr_attach_dev;
			genpd->detach_dev = cpg_mssr_detach_dev;
		}
	}

	genpd->power_off = rcar_sysc_pd_power_off;
	genpd->power_on = rcar_sysc_pd_power_on;

	if (pd->flags & (PD_CPU | PD_NO_CR)) {
		/* Skip CPUs (handled by SMP code) and areas without control */
		pr_debug("%s: Not touching %s\n", __func__, genpd->name);
		goto finalize;
	}

	if (!rcar_sysc_power_is_off(&pd->ch)) {
		pr_debug("%s: %s is already powered\n", __func__, genpd->name);
		goto finalize;
	}

	rcar_sysc_power(&pd->ch, true);

finalize:
	error = pm_genpd_init(genpd, &simple_qos_governor, false);
	if (error)
		pr_err("Failed to init PM domain %s: %d\n", name, error);

	return error;
}

struct rcar_sysc_pd *rcar_domains[RCAR_PD_ALWAYS_ON + 1];

static void rcar_power_on_force(void)
{
	int i;

	for (i = 0; i < RCAR_PD_ALWAYS_ON; i++) {
		struct rcar_sysc_pd *pd = rcar_domains[i];

		if (!pd)
			continue;

		if (rcar_sysc_quirks & BIT(pd->ch.isr_bit)) {
			if (!rcar_sysc_power_is_off(&pd->ch))
				continue;

			rcar_sysc_power(&pd->ch, true);
		}
	}
}

#ifdef CONFIG_PM_SLEEP
static void rcar_sysc_resume(void)
{
	pr_debug("%s\n", __func__);

	rcar_power_on_force();
}

static struct syscore_ops rcar_sysc_syscore_ops = {
	.resume = rcar_sysc_resume,
};
#endif

static const struct of_device_id rcar_sysc_matches[] __initconst = {
#ifdef CONFIG_SYSC_R8A7742
	{ .compatible = "renesas,r8a7742-sysc", .data = &r8a7742_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A7743
	{ .compatible = "renesas,r8a7743-sysc", .data = &r8a7743_sysc_info },
	/* RZ/G1N is identical to RZ/G2M w.r.t. power domains. */
	{ .compatible = "renesas,r8a7744-sysc", .data = &r8a7743_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A7745
	{ .compatible = "renesas,r8a7745-sysc", .data = &r8a7745_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A77470
	{ .compatible = "renesas,r8a77470-sysc", .data = &r8a77470_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A774A1
	{ .compatible = "renesas,r8a774a1-sysc", .data = &r8a774a1_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A774B1
	{ .compatible = "renesas,r8a774b1-sysc", .data = &r8a774b1_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A774C0
	{ .compatible = "renesas,r8a774c0-sysc", .data = &r8a774c0_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A774E1
	{ .compatible = "renesas,r8a774e1-sysc", .data = &r8a774e1_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A7779
	{ .compatible = "renesas,r8a7779-sysc", .data = &r8a7779_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A7790
	{ .compatible = "renesas,r8a7790-sysc", .data = &r8a7790_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A7791
	{ .compatible = "renesas,r8a7791-sysc", .data = &r8a7791_sysc_info },
	/* R-Car M2-N is identical to R-Car M2-W w.r.t. power domains. */
	{ .compatible = "renesas,r8a7793-sysc", .data = &r8a7791_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A7792
	{ .compatible = "renesas,r8a7792-sysc", .data = &r8a7792_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A7794
	{ .compatible = "renesas,r8a7794-sysc", .data = &r8a7794_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A7795
	{ .compatible = "renesas,r8a7795-sysc", .data = &r8a7795_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A77960
	{ .compatible = "renesas,r8a7796-sysc", .data = &r8a77960_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A77961
	{ .compatible = "renesas,r8a77961-sysc", .data = &r8a77961_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A77965
	{ .compatible = "renesas,r8a77965-sysc", .data = &r8a77965_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A77970
	{ .compatible = "renesas,r8a77970-sysc", .data = &r8a77970_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A77980
	{ .compatible = "renesas,r8a77980-sysc", .data = &r8a77980_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A77990
	{ .compatible = "renesas,r8a77990-sysc", .data = &r8a77990_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A77995
	{ .compatible = "renesas,r8a77995-sysc", .data = &r8a77995_sysc_info },
#endif
#ifdef CONFIG_SYSC_R8A779F0
	{ .compatible = "renesas,r8a779f0-sysc", .data = &r8a779f0_sysc_info },
#endif
	{ /* sentinel */ }
};

struct rcar_pm_domains {
	struct genpd_onecell_data onecell_data;
	struct generic_pm_domain *domains[RCAR_PD_ALWAYS_ON + 1];
};

static struct genpd_onecell_data *rcar_sysc_onecell_data;

/* Fix up power domain area in case PDMODE != 0 */
static void rcar_sysc_fixup_area(struct rcar_sysc_pd *pd, unsigned int mode)
{
	int i;

	/* Convert PDMODE to fix-up array position */
	mode = mode - 1;

	for (i = 0; i < NUM_FIXUP_AREAS; i++) {
		if (!strcmp(pd->genpd.name, r8a77980_fixup_areas[mode][i].name)) {
			pd->ch.chan_offs = r8a77980_fixup_areas[mode][i].chan_offs;
			pd->ch.chan_bit = r8a77980_fixup_areas[mode][i].chan_bit;
			pd->ch.isr_bit = r8a77980_fixup_areas[mode][i].isr_bit;
		}
	}
}

static int __init rcar_sysc_pd_init(void)
{
	const struct rcar_sysc_info *info;
	const struct of_device_id *match;
	struct rcar_pm_domains *domains;
	struct device_node *np;
	void __iomem *base;
	unsigned int i, mode;
	int error;
	const struct soc_device_attribute *attr;

	/* Implement for R-Car V3H only */
	if (soc_device_match(r8a77980))
		has_clk_crl = true;
	else
		has_clk_crl = false;

	np = of_find_matching_node_and_match(NULL, rcar_sysc_matches, &match);
	if (!np)
		return -ENODEV;

	info = match->data;

	if (info->init) {
		error = info->init();
		if (error)
			goto out_put;
	}

	has_cpg_mstp = of_find_compatible_node(NULL, NULL,
					       "renesas,cpg-mstp-clocks");

	attr = soc_device_match(rcar_sysc_quirks_match);
	if (attr)
		rcar_sysc_quirks = (uintptr_t)attr->data;

	base = of_iomap(np, 0);
	if (!base) {
		pr_warn("%pOF: Cannot map regs\n", np);
		error = -ENOMEM;
		goto out_put;
	}

	rcar_sysc_base = base;

	/* Optional External Request Mask Register */
	rcar_sysc_extmask_offs = info->extmask_offs;
	rcar_sysc_extmask_val = info->extmask_val;

	domains = kzalloc(sizeof(*domains), GFP_KERNEL);
	if (!domains) {
		error = -ENOMEM;
		goto out_put;
	}

	domains->onecell_data.domains = domains->domains;
	domains->onecell_data.num_domains = ARRAY_SIZE(domains->domains);
	rcar_sysc_onecell_data = &domains->onecell_data;

	if (info->mode)
		mode = *info->mode;
	else
		mode = 0;	/* No handle PDMODE */

	for (i = 0; i < info->num_areas; i++) {
		const struct rcar_sysc_area *area = &info->areas[i];
		struct rcar_sysc_pd *pd;

		if (!area->name) {
			/* Skip NULLified area */
			continue;
		}

		pd = kzalloc(sizeof(*pd) + strlen(area->name) + 1, GFP_KERNEL);
		if (!pd) {
			error = -ENOMEM;
			goto out_put;
		}

		strcpy(pd->name, area->name);
		pd->genpd.name = pd->name;
		pd->ch.chan_offs = area->chan_offs;
		pd->ch.chan_bit = area->chan_bit;
		pd->ch.isr_bit = area->isr_bit;
		pd->flags = area->flags;

		if (mode)
			rcar_sysc_fixup_area(pd, mode);

		if (rcar_sysc_quirks & BIT(pd->ch.isr_bit))
			pd->flags |= PD_NO_CR;

		error = rcar_sysc_pd_setup(pd);
		if (error)
			goto out_put;

		domains->domains[area->isr_bit] = &pd->genpd;
		rcar_domains[i] = pd;

		if (area->parent < 0)
			continue;

		error = pm_genpd_add_subdomain(domains->domains[area->parent],
					       &pd->genpd);
		if (error) {
			pr_warn("Failed to add PM subdomain %s to parent %u\n",
				area->name, area->parent);
			goto out_put;
		}
	}

	rcar_power_on_force();

	error = of_genpd_add_provider_onecell(np, &domains->onecell_data);

#ifdef CONFIG_PM_SLEEP
	if (!error)
		register_syscore_ops(&rcar_sysc_syscore_ops);
#endif

out_put:
	of_node_put(np);
	return error;
}
early_initcall(rcar_sysc_pd_init);

void __init rcar_sysc_nullify(struct rcar_sysc_area *areas,
			      unsigned int num_areas, u8 id)
{
	unsigned int i;

	for (i = 0; i < num_areas; i++)
		if (areas[i].isr_bit == id) {
			areas[i].name = NULL;
			return;
		}
}

#ifdef CONFIG_ARCH_R8A7779
static int rcar_sysc_power_cpu(unsigned int idx, bool on)
{
	struct generic_pm_domain *genpd;
	struct rcar_sysc_pd *pd;
	unsigned int i;

	if (!rcar_sysc_onecell_data)
		return -ENODEV;

	for (i = 0; i < rcar_sysc_onecell_data->num_domains; i++) {
		genpd = rcar_sysc_onecell_data->domains[i];
		if (!genpd)
			continue;

		pd = to_rcar_pd(genpd);
		if (!(pd->flags & PD_CPU) || pd->ch.chan_bit != idx)
			continue;

		return rcar_sysc_power(&pd->ch, on);
	}

	return -ENOENT;
}

int rcar_sysc_power_down_cpu(unsigned int cpu)
{
	return rcar_sysc_power_cpu(cpu, false);
}

int rcar_sysc_power_up_cpu(unsigned int cpu)
{
	return rcar_sysc_power_cpu(cpu, true);
}
#endif /* CONFIG_ARCH_R8A7779 */
