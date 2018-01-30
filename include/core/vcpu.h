/*
 * created by Le MIn 2017/12/09
 */

#ifndef _MVISOR_VCPU_H_
#define _MVISOR_VCPU_H_

#include <core/types.h>
#include <config/mvisor_config.h>
#include <core/list.h>
#include <core/vm.h>

typedef enum _vcpu_state_t {
	VCPU_STATE_READY 	= 0x0001,
	VCPU_STATE_RUNNING 	= 0x0002,
	VCPU_STATE_SLEEP 	= 0x0004,
	VCPU_STATE_STOP  	= 0x0008,
	VCPU_STATE_ERROR 	= 0xffff,
} vcpu_state_t;

#ifdef CONFIG_ARM_AARCH64

typedef struct vmm_vcpu_context {
	uint64_t x0;
	uint64_t x1;
	uint64_t x2;
	uint64_t x3;
	uint64_t x4;
	uint64_t x5;
	uint64_t x6;
	uint64_t x7;
	uint64_t x8;
	uint64_t x9;
	uint64_t x10;
	uint64_t x11;
	uint64_t x12;
	uint64_t x13;
	uint64_t x14;
	uint64_t x15;
	uint64_t x16;
	uint64_t x17;
	uint64_t x18;
	uint64_t x19;
	uint64_t x20;
	uint64_t x21;
	uint64_t x22;
	uint64_t x23;
	uint64_t x24;
	uint64_t x25;
	uint64_t x26;
	uint64_t x27;
	uint64_t x28;
	uint64_t x29;
	uint64_t x30_lr;
	uint64_t sp_el1;
	uint64_t elr_el2;
	uint64_t vbar_el1;
	uint64_t spsr_el2;
	uint64_t nzcv;
	uint64_t esr_el1;
	uint64_t vmpidr;
	uint64_t sctlr_el1;
	uint64_t ttbr0_el1;
	uint64_t ttbr1_el1;
	uint64_t vttbr_el2;
	uint64_t vtcr_el2;
	uint64_t hcr_el2;
	uint64_t ich_ap0r0_el2;
	uint64_t ich_ap1r0_el2;
	uint64_t ich_eisr_el2;
	uint64_t ich_elrsr_el2;
	uint64_t ich_hcr_el2;
	uint64_t ich_lr0_el2;
	uint64_t ich_lr1_el2;
	uint64_t ich_lr2_el2;
	uint64_t ich_lr3_el2;
	uint64_t ich_lr4_el2;
	uint64_t ich_lr5_el2;
	uint64_t ich_lr6_el2;
	uint64_t ich_lr7_el2;
	uint64_t ich_lr8_el2;
	uint64_t ich_lr9_el2;
	uint64_t ich_lr10_el2;
	uint64_t ich_lr11_el2;
	uint64_t ich_lr12_el2;
	uint64_t ich_lr13_el2;
	uint64_t ich_lr14_el2;
	uint64_t ich_lr15_el2;
	uint64_t ich_misr_el2;
	uint64_t ich_vmcr_el2;
	uint64_t ich_vtr_el2;
	uint64_t icv_ap0r0_el1;
	uint64_t icv_ap1r0_el1;
	uint64_t icv_bpr0_el1;
	uint64_t icv_bpr1_el1;
	uint64_t icv_ctlr_el1;
	uint64_t icv_dir_el1;
	uint64_t icv_eoir0_el1;
	uint64_t icv_eoir1_el1;
	uint64_t icv_hppir0_el1;
	uint64_t icv_hppir1_el1;
	uint64_t icv_iar0_el1;
	uint64_t icv_iar1_el1;
	uint64_t icv_igrpen0_el1;
	uint64_t icv_igrpen1_el1;
	uint64_t icv_pmr_el1;
	uint64_t icv_rpr_el1;
} vcpu_context_t __attribute__ ((__aligned__ (sizeof(unsigned long))));

#else

typedef vmm_vcpu_context {

} vcpu_context_t ;

#endif

typedef struct vmm_vcpu {
	vcpu_context_t context;
	uint32_t vcpu_id;
	vcpu_state_t state;
	vm_t *vm_belong_to;
	phy_addr_t entry_point;
	uint32_t pcpu_affinity;
	uint32_t status;
	struct list_head pcpu_list;
} vcpu_t __attribute__ ((__aligned__ (sizeof(unsigned long))));

static vcpu_state_t inline vmm_get_vcpu_state(vcpu_t *vcpu)
{
	return vcpu->state;
}

static void inline vmm_set_vcpu_state(vcpu_t *vcpu, vcpu_state_t state)
{
	vcpu->state = state;
}

static uint32_t inline vmm_get_vcpu_id(vcpu_t *vcpu)
{
	return vcpu->vcpu_id;
}

static uint32_t inline vmm_get_vm_id(vcpu_t *vcpu)
{
	return (vcpu->vm_belong_to->vmid);
}

static uint32_t inline vmm_get_pcpu_id(vcpu_t *vcpu)
{
	return vcpu->pcpu_affinity;
}

vcpu_t *vmm_get_vcpu(uint32_t vmid, uint32_t vcpu_id);

#endif
