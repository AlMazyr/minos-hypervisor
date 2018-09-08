/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/minos.h>
#include <minos/io.h>
#include <minos/percpu.h>
#include <minos/spinlock.h>
#include <minos/print.h>
#include <asm/gicv3.h>
#include <minos/errno.h>
#include <minos/vmodule.h>
#include <minos/vcpu.h>
#include <asm/arch.h>
#include <minos/cpumask.h>
#include <minos/irq.h>
#include <minos/virq.h>
#include <asm/of.h>

spinlock_t gicv3_lock;
static void *gicd_base = 0;

static int gicv3_nr_lr = 0;
static int gicv3_nr_pr = 0;

DEFINE_PER_CPU(void *, gicr_rd_base);
DEFINE_PER_CPU(void *, gicr_sgi_base);

#define gicr_rd_base()	get_cpu_var(gicr_rd_base)
#define gicr_sgi_base()	get_cpu_var(gicr_sgi_base)

static void gicv3_gicd_wait_for_rwp(void)
{
	while (ioread32(gicd_base + GICD_CTLR) & (1 << 31));
}

static void gicv3_gicr_wait_for_rwp(void)
{
	while (ioread32(gicr_rd_base() + GICR_CTLR) & (1 << 3));
}

static void gicv3_mask_irq(uint32_t irq)
{
	uint32_t mask = 1 << (irq % 32);

	spin_lock(&gicv3_lock);
	if (irq < GICV3_NR_LOCAL_IRQS) {
		iowrite32(gicr_sgi_base() + GICR_ICENABLER + (irq / 32) * 4, mask);
		gicv3_gicr_wait_for_rwp();
	} else {
		iowrite32(gicd_base + GICD_ICENABLER + (irq / 32) * 4, mask);
		gicv3_gicd_wait_for_rwp();
	}
	spin_unlock(&gicv3_lock);
}

static void gicv3_eoi_irq(uint32_t irq)
{
	write_sysreg32(irq, ICC_EOIR1_EL1);
	isb();
}

static void gicv3_dir_irq(uint32_t irq)
{
	write_sysreg32(irq, ICC_DIR_EL1);
	isb();
}

static uint32_t gicv3_read_irq(void)
{
	uint32_t irq;

	irq = read_sysreg32(ICC_IAR1_EL1);
	dsbsy();
	return irq;
}

static int gicv3_set_irq_type(uint32_t irq, uint32_t type)
{
	void *base;
	uint32_t cfg, edgebit;
	
	spin_lock(&gicv3_lock);

	if (irq >= GICV3_NR_LOCAL_IRQS)
		base = (void *)gicd_base + GICD_ICFGR + (irq / 16) * 4;
	else
		base = (void *)gicr_sgi_base() + GICR_ICFGR1;

	cfg = ioread32(base);
	edgebit = 2u << (2 * (irq % 16));
	if (type & IRQ_FLAGS_LEVEL_BOTH)
		cfg &= ~edgebit;
	else if (type & IRQ_FLAGS_EDGE_BOTH)
		cfg |= edgebit;

	iowrite32(base, cfg);

	spin_unlock(&gicv3_lock);

	return 0;
}

static void gicv3_clear_pending(uint32_t irq)
{
	uint32_t offset, bit;

	spin_lock(&gicv3_lock);

	if (irq >= GICV3_NR_LOCAL_IRQS) {
		iowrite32((void *)gicr_sgi_base() + GICR_ICPENDR0, BIT(irq));
	} else {
		irq = irq - 32;
		offset = irq / 32;
		bit = offset % 32;
		iowrite32((void *)gicd_base + GICD_ICPENDR \
				+ (offset * 4), BIT(bit));
	}

	spin_unlock(&gicv3_lock);
}

static int gicv3_set_irq_priority(uint32_t irq, uint32_t pr)
{
	spin_lock(&gicv3_lock);

	if (irq < GICV3_NR_LOCAL_IRQS)
		iowrite8(gicr_sgi_base() + GICR_IPRIORITYR0 + irq, pr);
	else
		iowrite8(gicd_base + GICD_IPRIORITYR + irq, pr);
	
	spin_unlock(&gicv3_lock);

	return 0;
}

static int gicv3_set_irq_affinity(uint32_t irq, uint32_t pcpu)
{
	uint64_t affinity;

	affinity = logic_cpu_to_irq_affinity(pcpu);
	affinity &= ~(1 << 31); //GICD_IROUTER_SPI_MODE_ANY

	spin_lock(&gicv3_lock);
	iowrite64(gicd_base + GICD_IROUTER + irq * 8, affinity);
	spin_unlock(&gicv3_lock);

	return 0;
}

static void gicv3_send_sgi_list(uint32_t sgi, cpumask_t *mask)
{
	uint64_t list = 0;
	uint64_t val;
	int cpu;

	for_each_cpu(cpu, mask)
		list |= (1 << cpu);

	/*
	 * TBD: now only support one cluster
	 */
	val = list | (0ul << 16) | (0ul << 32) |
		(0ul << 48) | (sgi << 24);
	write_sysreg64(val, ICC_SGI1R_EL1);
	isb();
}

static uint64_t gicv3_read_lr(int lr)
{
	switch (lr) {
	case 0: return read_sysreg(ICH_LR0_EL2);
	case 1: return read_sysreg(ICH_LR1_EL2);
	case 2: return read_sysreg(ICH_LR2_EL2);
	case 3: return read_sysreg(ICH_LR3_EL2);
	case 4: return read_sysreg(ICH_LR4_EL2);
	case 5: return read_sysreg(ICH_LR5_EL2);
	case 6: return read_sysreg(ICH_LR6_EL2);
	case 7: return read_sysreg(ICH_LR7_EL2);
	case 8: return read_sysreg(ICH_LR8_EL2);
	case 9: return read_sysreg(ICH_LR9_EL2);
	case 10: return read_sysreg(ICH_LR10_EL2);
	case 11: return read_sysreg(ICH_LR11_EL2);
	case 12: return read_sysreg(ICH_LR12_EL2);
	case 13: return read_sysreg(ICH_LR13_EL2);
	case 14: return read_sysreg(ICH_LR14_EL2);
	case 15: return read_sysreg(ICH_LR15_EL2);
	default:
		 return 0;
	}
}

static void gicv3_write_lr(int lr, uint64_t val)
{
	switch ( lr )
	{
	case 0:
		write_sysreg(val, ICH_LR0_EL2);
		break;
	case 1:
		write_sysreg(val, ICH_LR1_EL2);
		break;
	case 2:
		write_sysreg(val, ICH_LR2_EL2);
		break;
	case 3:
		write_sysreg(val, ICH_LR3_EL2);
		break;
	case 4:
		write_sysreg(val, ICH_LR4_EL2);
		break;
	case 5:
		write_sysreg(val, ICH_LR5_EL2);
		break;
	case 6:
		write_sysreg(val, ICH_LR6_EL2);
		break;
	case 7:
		write_sysreg(val, ICH_LR7_EL2);
		break;
	case 8:
		write_sysreg(val, ICH_LR8_EL2);
		break;
	case 9:
		write_sysreg(val, ICH_LR9_EL2);
		break;
	case 10:
		write_sysreg(val, ICH_LR10_EL2);
		break;
	case 11:
		write_sysreg(val, ICH_LR11_EL2);
		break;
	case 12:
		write_sysreg(val, ICH_LR12_EL2);
		break;
	case 13:
		write_sysreg(val, ICH_LR13_EL2);
		break;
	case 14:
		write_sysreg(val, ICH_LR14_EL2);
		break;
	case 15:
		write_sysreg(val, ICH_LR15_EL2);
		break;
	default:
		return;
	}

	isb();
}

static int gicv3_send_virq(struct virq *virq)
{
	uint64_t value = 0;
	struct gic_lr *lr = (struct gic_lr *)&value;

	lr->v_intid = virq->v_intno;
	lr->p_intid = virq->h_intno;
	lr->priority = virq->pr;
	lr->group = 1;
	lr->hw = virq->hw;
	lr->state = 1;

	gicv3_write_lr(virq->id, value);

	return 0;
}

static int gicv3_update_virq(struct virq *virq, int action)
{
	switch (action) {
		/*
		 * wether need to update the context value?
		 * TBD, since the context has not been saved
		 * so do not need to update it.
		 *
		 * 2: if the virq is attached to a physical irq
		 *    need to update the GICR register ?
		 */

	case VIRQ_ACTION_REMOVE:
		if (virq->hw)
			gicv3_clear_pending(virq->h_intno);

	case VIRQ_ACTION_CLEAR:
		gicv3_write_lr(virq->id, 0);
		break;

	default:
		break;
	}

	return 0;
}

static void gicv3_send_sgi(uint32_t sgi, enum sgi_mode mode, cpumask_t *cpu)
{
	cpumask_t cpus_mask;

	if (sgi > 15)
		return;

	cpumask_clear(&cpus_mask);

	switch (mode) {
	case SGI_TO_OTHERS:
		write_sysreg64(ICH_SGI_TARGET_OTHERS << ICH_SGI_IRQMODE_SHIFT |
				(uint64_t)sgi << ICH_SGI_IRQ_SHIFT, ICC_SGI1R_EL1);
		isb();
		break;
	case SGI_TO_SELF:
		cpumask_set_cpu(smp_processor_id(), &cpus_mask);
		gicv3_send_sgi_list(sgi, &cpus_mask);
		break;
	case SGI_TO_LIST:
		gicv3_send_sgi_list(sgi, cpu);
		break;
	default:
		pr_error("Sgi mode not supported\n");
		break;
	}
}

static void gicv3_unmask_irq(uint32_t irq)
{
	uint32_t mask = 1 << (irq % 32);

	spin_lock(&gicv3_lock);

	if (irq < GICV3_NR_LOCAL_IRQS) {
		iowrite32(gicr_sgi_base() + GICR_ISENABLER + (irq / 32) * 4, mask);
		gicv3_gicr_wait_for_rwp();
	} else {
		iowrite32(gicd_base + GICD_ISENABLER + (irq / 32) * 4, mask);
		gicv3_gicd_wait_for_rwp();
	}

	spin_unlock(&gicv3_lock);
}

static void gicv3_wakeup_gicr(void)
{
	uint32_t gicv3_waker_value;

	gicv3_waker_value = ioread32(gicr_rd_base() + GICR_WAKER);
	gicv3_waker_value &= ~(GICR_WAKER_PROCESSOR_SLEEP); 
	iowrite32(gicr_rd_base() + GICR_WAKER, gicv3_waker_value);

	while ((ioread32(gicr_rd_base() + GICR_WAKER)
			& GICR_WAKER_CHILDREN_ASLEEP) != 0);
}

uint32_t gicv3_get_irq_num(void)
{
	uint32_t type;

	type = ioread32(gicd_base + GICD_TYPER);

	return (32 * ((type & 0x1f)));
}

int gicv3_get_virq_state(struct virq *virq)
{
	uint64_t value;

	value = gicv3_read_lr(virq->id);
	value = (value & 0xc000000000000000) >> 62;

	return ((int)value);
}

static int gicv3_gicc_init(void)
{
	unsigned long reg_value;

	/* enable sre */
	reg_value = read_icc_sre_el2();
	reg_value |= (1 << 0);
	write_icc_sre_el2(reg_value);

	write_sysreg32(0, ICC_BPR1_EL1);
	write_sysreg32(0xff, ICC_PMR_EL1);
	write_sysreg32(1 << 1, ICC_CTLR_EL1);
	write_sysreg32(1, ICC_IGRPEN1_EL1);
	isb();

	return 0;
}

static int gicv3_hyp_init(void)
{
	write_sysreg32(GICH_VMCR_VENG1 | (0xff << 24), ICH_VMCR_EL2);
	write_sysreg32(GICH_HCR_EN, ICH_HCR_EL2);

	/*
	 * set IMO and FMO let physic irq and fiq taken to
	 * EL2, without this irq and fiq will not send to
	 * the cpu
	 */
	write_sysreg64(HCR_EL2_IMO | HCR_EL2_FMO, HCR_EL2);

	isb();
	return 0;
}

static int gicv3_gicr_init(void)
{
	int i;
	uint64_t pr;

	gicv3_wakeup_gicr();

	/* set the priority on PPI and SGI */
	pr = (0x90 << 24) | (0x90 << 16) | (0x90 << 8) | 0x90;
	for (i = 0; i < GICV3_NR_SGI; i += 4)
		iowrite32(gicr_sgi_base() + GICR_IPRIORITYR0 + (i / 4) * 4, pr);

	pr = (0xa0 << 24) | (0xa0 << 16) | (0xa0 << 8) | 0xa0;
	for (i = GICV3_NR_SGI; i < GICV3_NR_LOCAL_IRQS; i += 4)
		iowrite32(gicr_sgi_base() + GICR_IPRIORITYR0 + (i / 4) * 4, pr);

	/* disable all PPI and enable all SGI */
	iowrite32(gicr_sgi_base() + GICR_ICENABLER, 0xffff0000);
	iowrite32(gicr_sgi_base() + GICR_ISENABLER, 0x0000ffff);

	/* configure SGI and PPI as non-secure Group-1 */
	iowrite32(gicr_sgi_base() + GICR_IGROUPR0, 0xffffffff);

	gicv3_gicr_wait_for_rwp();
	isb();

	return 0;
}

static void gicv3_save_lrs(struct gic_context *c, uint32_t count)
{
	if (count > 16)
		panic("Unsupport LR count\n");

	switch (count) {
	case 16:
		c->ich_lr15_el2 = read_sysreg(ICH_LR15_EL2);
	case 15:
		c->ich_lr14_el2 = read_sysreg(ICH_LR14_EL2);
	case 14:
		c->ich_lr13_el2 = read_sysreg(ICH_LR13_EL2);
	case 13:
		c->ich_lr12_el2 = read_sysreg(ICH_LR12_EL2);
	case 12:
		c->ich_lr11_el2 = read_sysreg(ICH_LR11_EL2);
	case 11:
		c->ich_lr10_el2 = read_sysreg(ICH_LR10_EL2);
	case 10:
		c->ich_lr9_el2 = read_sysreg(ICH_LR9_EL2);
	case 9:
		c->ich_lr8_el2 = read_sysreg(ICH_LR8_EL2);
	case 8:
		c->ich_lr7_el2 = read_sysreg(ICH_LR7_EL2);
	case 7:
		c->ich_lr6_el2 = read_sysreg(ICH_LR6_EL2);
	case 6:
		c->ich_lr5_el2 = read_sysreg(ICH_LR5_EL2);
	case 5:
		c->ich_lr4_el2 = read_sysreg(ICH_LR4_EL2);
	case 4:
		c->ich_lr3_el2 = read_sysreg(ICH_LR3_EL2);
	case 3:
		c->ich_lr2_el2 = read_sysreg(ICH_LR2_EL2);
	case 2:
		c->ich_lr1_el2 = read_sysreg(ICH_LR1_EL2);
	case 1:
		c->ich_lr0_el2 = read_sysreg(ICH_LR0_EL2);
		break;
	default:
		break;
	}
}

static void gicv3_save_aprn(struct gic_context *c, uint32_t count)
{
	switch (count) {
	case 7:
		c->ich_ap0r2_el2 = read_sysreg32(ICH_AP0R2_EL2);
		c->ich_ap1r2_el2 = read_sysreg32(ICH_AP1R2_EL2);
	case 6:
		c->ich_ap0r1_el2 = read_sysreg32(ICH_AP0R1_EL2);
		c->ich_ap1r1_el2 = read_sysreg32(ICH_AP1R1_EL2);
	case 5:
		c->ich_ap0r0_el2 = read_sysreg32(ICH_AP0R0_EL2);
		c->ich_ap1r0_el2 = read_sysreg32(ICH_AP1R0_EL2);
		break;
	default:
		panic("Unsupport aprn count\n");
	}
}

static void gicv3_state_save(struct vcpu *vcpu, void *context)
{
	struct gic_context *c = (struct gic_context *)context;

	dsb();
	gicv3_save_lrs(c, gicv3_nr_lr);
	gicv3_save_aprn(c, gicv3_nr_pr);
	c->icc_sre_el1 = read_sysreg32(ICC_SRE_EL1);
	c->ich_vmcr_el2 = read_sysreg32(ICH_VMCR_EL2);
	c->ich_hcr_el2 = read_sysreg32(ICH_HCR_EL2);
}

static void gicv3_restore_aprn(struct gic_context *c, uint32_t count)
{
	switch (count) {
	case 7:
		write_sysreg32(c->ich_ap0r2_el2, ICH_AP0R2_EL2);
		write_sysreg32(c->ich_ap1r2_el2, ICH_AP1R2_EL2);
	case 6:
		write_sysreg32(c->ich_ap0r1_el2, ICH_AP0R1_EL2);
		write_sysreg32(c->ich_ap1r1_el2, ICH_AP1R1_EL2);
	case 5:
		write_sysreg32(c->ich_ap0r0_el2, ICH_AP0R0_EL2);
		write_sysreg32(c->ich_ap1r0_el2, ICH_AP1R0_EL2);
		break;
	default:
		panic("Unsupport aprn count");
	}
}

static void gicv3_restore_lrs(struct gic_context *c, uint32_t count)
{
	if (count > 16)
		panic("Unsupport LR count");

	switch (count) {
	case 16:
		write_sysreg(c->ich_lr15_el2, ICH_LR15_EL2);
	case 15:
		write_sysreg(c->ich_lr14_el2, ICH_LR14_EL2);
	case 14:
		write_sysreg(c->ich_lr13_el2, ICH_LR13_EL2);
	case 13:
		write_sysreg(c->ich_lr12_el2, ICH_LR12_EL2);
	case 12:
		write_sysreg(c->ich_lr11_el2, ICH_LR11_EL2);
	case 11:
		write_sysreg(c->ich_lr10_el2, ICH_LR10_EL2);
	case 10:
		write_sysreg(c->ich_lr9_el2, ICH_LR9_EL2);
	case 9:
		write_sysreg(c->ich_lr8_el2, ICH_LR8_EL2);
	case 8:
		write_sysreg(c->ich_lr7_el2, ICH_LR7_EL2);
	case 7:
		write_sysreg(c->ich_lr6_el2, ICH_LR6_EL2);
	case 6:
		write_sysreg(c->ich_lr5_el2, ICH_LR5_EL2);
	case 5:
		write_sysreg(c->ich_lr4_el2, ICH_LR4_EL2);
	case 4:
		write_sysreg(c->ich_lr3_el2, ICH_LR3_EL2);
	case 3:
		write_sysreg(c->ich_lr2_el2, ICH_LR2_EL2);
	case 2:
		write_sysreg(c->ich_lr1_el2, ICH_LR1_EL2);
	case 1:
		write_sysreg(c->ich_lr0_el2, ICH_LR0_EL2);
		break;
	default:
		break;
	}
}

static void gicv3_state_restore(struct vcpu *vcpu, void *context)
{
	struct gic_context *c = (struct gic_context *)context;

	gicv3_restore_lrs(c, gicv3_nr_lr);
	gicv3_restore_aprn(c, gicv3_nr_pr);
	write_sysreg32(c->icc_sre_el1, ICC_SRE_EL1);
	write_sysreg32(c->ich_vmcr_el2, ICH_VMCR_EL2);
	write_sysreg32(c->ich_hcr_el2, ICH_HCR_EL2);
	dsb();
}

static void gicv3_state_init(struct vcpu *vcpu, void *context)
{
	struct gic_context *c = (struct gic_context *)context;

	memset((char *)c, 0, sizeof(struct gic_context));
	c->icc_sre_el1 = 0x7;
	c->ich_vmcr_el2 = GICH_VMCR_VENG1 | (0xff << 24);
	c->ich_hcr_el2 = GICH_HCR_EN;
}

int gicv3_init(void)
{
	int i;
	uint32_t type;
	uint32_t nr_lines;
	void *rbase;
	uint64_t pr;
	uint32_t value;
	uint64_t array[16];
	void * __gicr_rd_base = 0;

	pr_info("*** gicv3 init ***\n");

	spin_lock_init(&gicv3_lock);

	memset(array, 0, sizeof(array));
	type = of_get_u64_array("/interrupt-controller",
				"reg", array, &i);
	if (type || i < 4)
		panic("can not find gicv3 interrupt controller\n");

	/* only map gicd and gicr now */
	pr = array[2] + array[3] - array[0];
	io_remap(array[0], array[0], pr);

	gicd_base = (void *)array[0];
	__gicr_rd_base = (void *)array[2];

	value = read_sysreg32(ICH_VTR_EL2);
	gicv3_nr_lr = (value & 0x3f) + 1;
	gicv3_nr_pr = ((value >> 29) & 0x7) + 1;

	if (!((gicv3_nr_pr > 4) && (gicv3_nr_pr < 8)))
		panic("GICv3: Invalid number of priority bits\n");

	for (i = 0; i < CONFIG_NR_CPUS; i++) {
		rbase = __gicr_rd_base + (128 * 1024) * i;
		get_per_cpu(gicr_rd_base, i) = rbase;
		get_per_cpu(gicr_sgi_base, i) = rbase + (64 * 1024);
	}

	spin_lock(&gicv3_lock);

	/* disable gicd */
	iowrite32(gicd_base + GICD_CTLR, 0);

	type = ioread32(gicd_base + GICD_TYPER);
	pr_info("typer reg of gicv3 is 0x%x\n", type);
	nr_lines = 32 * ((type & 0x1f));

	/* alloc LOCAL_IRQS for each cpus */
	irq_alloc_sgi(0, 16);
	irq_alloc_ppi(16, 16);

	/* alloc SPI irqs */
	irq_alloc_spi(32, nr_lines);
	
	/* default all golbal IRQS to level, active low */
	for (i = GICV3_NR_LOCAL_IRQS; i < nr_lines; i += 16)
		iowrite32(gicd_base + GICD_ICFGR + (i / 16) * 4, 0);

	/* default priority for global interrupts */
	for (i = GICV3_NR_LOCAL_IRQS; i < nr_lines; i += 4) {
		pr = (0xa0 << 24) | (0xa0 << 16) | (0xa0 << 8) | 0xa0;
		iowrite32(gicd_base + GICD_IPRIORITYR + (i / 4) * 4, pr);
		pr = ioread32(gicd_base + GICD_IPRIORITYR + (i / 4) * 4);
	}

	/* disable all global interrupt */
	for (i = GICV3_NR_LOCAL_IRQS; i < nr_lines; i += 32)
		iowrite32(gicd_base + GICD_ICENABLER + (i / 32) *4, 0xffffffff);

	/* configure SPIs as non-secure GROUP-1 */
	for (i = GICV3_NR_LOCAL_IRQS; i < nr_lines; i += 32)
		iowrite32(gicd_base + GICD_IGROUPR + (i / 32) *4, 0xffffffff);

	gicv3_gicd_wait_for_rwp();

	/* enable the gicd */
	iowrite32(gicd_base + GICD_CTLR, 1 | GICD_CTLR_ENABLE_GRP1 |
			GICD_CTLR_ENABLE_GRP1A | GICD_CTLR_ARE_NS);
	isb();

	gicv3_gicr_init();
	gicv3_gicc_init();
	gicv3_hyp_init();

	spin_unlock(&gicv3_lock);

	return 0;
}

int gicv3_secondary_init(void)
{
	gicv3_gicr_init();
	gicv3_gicc_init();
	gicv3_hyp_init();

	return 0;
}

static struct irq_chip gicv3_chip = {
	.irq_mask 		= gicv3_mask_irq,
	.irq_unmask 		= gicv3_unmask_irq,
	.irq_eoi 		= gicv3_eoi_irq,
	.irq_dir		= gicv3_dir_irq,
	.irq_set_type 		= gicv3_set_irq_type,
	.irq_set_affinity 	= gicv3_set_irq_affinity,
	.send_sgi		= gicv3_send_sgi,
	.get_pending_irq	= gicv3_read_irq,
	.irq_set_priority	= gicv3_set_irq_priority,
	.get_virq_state		= gicv3_get_virq_state,
	.send_virq		= gicv3_send_virq,
	.update_virq		= gicv3_update_virq,
	.init			= gicv3_init,
	.secondary_init		= gicv3_secondary_init,
};

static int gicv3_vmodule_init(struct vmodule *vmodule)
{
	vmodule->context_size = sizeof(struct gic_context);
	vmodule->pdata = NULL;
	vmodule->state_init = gicv3_state_init;
	vmodule->state_save = gicv3_state_save;
	vmodule->state_restore = gicv3_state_restore;

	return 0;
}

MINOS_MODULE_DECLARE(gicv3, "gicv3-vmodule", (void *)gicv3_vmodule_init);
IRQCHIP_DECLARE(gicv3_chip, "gicv3", (void *)&gicv3_chip);
