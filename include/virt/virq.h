#ifndef __MINOS_VIRQ_H__
#define __MINOS_VIRQ_H__

#include <minos/spinlock.h>
#include <minos/types.h>
#include <minos/cpumask.h>

struct vcpu;
struct virqtag;

#define CONFIG_VCPU_MAX_ACTIVE_IRQS	(16)

#define VIRQ_STATE_INACTIVE		(0x0)
#define VIRQ_STATE_PENDING		(0x1)
#define VIRQ_STATE_ACTIVE		(0x2)
#define VIRQ_STATE_ACTIVE_AND_PENDING	(0x3)
#define VIRQ_STATE_OFFLINE		(0x4)

#define VIRQ_ACTION_REMOVE	(0x0)
#define VIRQ_ACTION_ADD		(0x1)
#define VIRQ_ACTION_CLEAR	(0x2)

#define VIRQ_BASE		(1024)
#define MAX_VIRQ_NR		(512)
#define VIRQ_OFFSET(v)		(v - VIRQ_BASE)

#define VCPU_MAX_LOCAL_IRQS	(32)

struct virq {
	uint32_t h_intno;
	uint32_t v_intno;
	uint8_t hw;
	uint8_t state;
	uint16_t id;
	uint16_t pr;
	struct list_head list;
};

struct virq_struct {
	uint32_t active_count;
	uint32_t pending_count;
	spinlock_t lock;
	struct list_head pending_list;
	DECLARE_BITMAP(irq_bitmap, CONFIG_VCPU_MAX_ACTIVE_IRQS);
	DECLARE_BITMAP(local_irq_mask, VCPU_MAX_LOCAL_IRQS);
	struct virq virqs[CONFIG_VCPU_MAX_ACTIVE_IRQS];
};

int __virq_enable(uint32_t virq, int enable);
void vcpu_virq_struct_init(struct virq_struct *irq_struct);

int send_virq_hw(uint32_t vmid, uint32_t virq, uint32_t hirq);
int send_virq_to_vm(uint32_t vmid, uint32_t virq);
int send_virq_to_vcpu(struct vcpu *vcpu, uint32_t virq);
void send_vsgi(struct vcpu *sender,
		uint32_t sgi, cpumask_t *cpumask);
int vcpu_has_virq_pending(struct vcpu *vcpu);
int vcpu_has_virq_active(struct vcpu *vcpu);
int vcpu_has_virq(struct vcpu *vcpu);
void clear_pending_virq(uint32_t irq);
int register_virq(struct virqtag *v);
int virq_set_priority(uint32_t virq, int pr);

int alloc_virtual_irqs(uint32_t start, uint32_t count, int type);

static inline void virq_mask(uint32_t virq)
{
	__virq_enable(virq, 0);
}

static inline void virq_unmask(uint32_t virq)
{
	__virq_enable(virq, 1);
}

#endif
