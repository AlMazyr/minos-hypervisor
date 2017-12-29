#include <core/core.h>
#include <core/mmu.h>
#include <asm/armv8.h>

#define G4K_LEVEL1_OFFSET	(30)
#define G4K_LEVEL2_OFFSET	(21)
#define G16K_LEVEL1_OFFSET	(25)
#define G16K_LEVEL2_OFFSET	(14)
#define G64K_LEVEL1_OFFSET	(29)
#define G64K_LEVEL2_OFFSET	(16)

#ifdef CONFIG_GRANULE_SIZE_4K
	#define GRANULE_TYPE		GRANULE_SIZE_4K
	#define LEVEL2_OFFSET		G4K_LEVEL2_OFFSET
	#define LEVEL1_OFFSET		G4K_LEVEL1_OFFSET
	#define LEVEL2_ENTRY_MAP_SIZE	(SIZE_2M)
	#define LEVEL1_ENTRY_MAP_SIZE	(SIZE_1G)
	#define LEVEL1_DESCRIPTION_TYPE	(DESCRIPTION_TABLE)
	#define LEVEL2_DESCRIPTION_TYPE	(DESCRIPTION_BLOCK)
	#define LEVEL2_PAGES		(1)
#elif CONFIG_GRANULE_SIZE_16K
	#define GRANULE_TYPE		GRANULE_SIZE_16K
	#define LEVEL2_OFFSET		G16K_LEVEL2_OFFSET
	#define LEVEL1_OFFSET		G16K_LEVEL1_OFFSET
	#define LEVEL2_OFFSET		G16K_LEVEL2_OFFSET
	#define LEVEL1_OFFSET		G16K_LEVEL1_OFFSET
	#define LEVEL2_ENTRY_MAP_SIZE	(SIZE_16K)
	#define LEVEL1_ENTRY_MAP_SIZE	(SIZE_32M)
	#define LEVEL1_DESCRIPTION_TYPE	(DESCRIPTION_TABLE)
	#define LEVEL2_DESCRIPTION_TYPE	(DESCRIPTION_BLOCK)
	#define LEVEL2_PAGES		(4)
#else
	#define GRANULE_TYPE		GRANULE_SIZE_16K
	#define LEVEL2_OFFSET		G64K_LEVEL2_OFFSET
	#define LEVEL1_OFFSET		G64K_LEVEL1_OFFSET
	#define LEVEL2_OFFSET		G64K_LEVEL2_OFFSET
	#define LEVEL1_OFFSET		G64K_LEVEL1_OFFSET
	#define LEVEL2_ENTRY_MAP_SIZE	(SIZE_64K)
	#define LEVEL1_ENTRY_MAP_SIZE	(SIZE_512M)
	#define LEVEL1_DESCRIPTION_TYPE	(DESCRIPTION_TABLE)
	#define LEVEL2_DESCRIPTION_TYPE	(DESCRIPTION_BLOCK)
	#define LEVEL2_PAGES		(16)
#endif

struct aa64mmfr0 {
	uint64_t pa_range : 4;
	uint64_t asid : 4;
	uint64_t big_end : 4;
	uint64_t sns_mem : 4;
	uint64_t big_end_el0 : 4;
	uint64_t t_gran_16k : 4;
	uint64_t t_gran_64k : 4;
	uint64_t t_gran_4k : 4;
	uint64_t res : 32;
};

uint64_t get_tt_description(int m_type, int d_type)
{
	uint64_t attr;

	if (d_type == DESCRIPTION_TABLE)
		return (uint64_t)TT_S2_ATTR_TABLE;

	if ((d_type == DESCRIPTION_BLOCK) || (d_type == DESCRIPTION_PAGE)) {
		if (m_type == MEM_TYPE_NORMAL) {
			attr = TT_S2_ATTR_BLOCK | TT_S2_ATTR_AP_RW | \
			     	TT_S2_ATTR_SH_INNER | TT_S2_ATTR_AF | \
				TT_S2_ATTR_MEMATTR_OUTER_WT | \
				TT_S2_ATTR_MEMATTR_NORMAL_INNER_WT;
		} else {
			attr = TT_S2_ATTR_BLOCK | TT_S2_ATTR_AP_RW | \
			       TT_S2_ATTR_SH_INNER | TT_S2_ATTR_AF | \
			       TT_S2_ATTR_MEMATTR_DEVICE | \
			       TT_S2_ATTR_MEMATTR_DEV_nGnRnE;
		}

		return attr;
	}

	return 0;
}

static int mmu_map_level2_pages(phy_addr_t *tbase, phy_addr_t vbase,
		phy_addr_t pbase, size_t size, int type)
{
	int i;
	uint64_t attr;
	uint32_t offset;
	phy_addr_t tmp;

	attr = get_tt_description(type, LEVEL2_DESCRIPTION_TYPE);
	tmp = ALIGN(vbase, LEVEL1_ENTRY_MAP_SIZE);
	offset = (vbase - tmp) >> LEVEL2_OFFSET;

	for (i = 0; i < (size >> LEVEL2_OFFSET); i++) {
		*(tbase + offset) = (attr | \
			(pbase >> LEVEL2_OFFSET) << LEVEL2_OFFSET);
		vbase += LEVEL2_ENTRY_MAP_SIZE;
		pbase += LEVEL2_ENTRY_MAP_SIZE;
		offset++;
	}

	return 0;
}

int mmu_map_mem(phy_addr_t *tbase, phy_addr_t base, size_t size, int type)
{
	int i;
	phy_addr_t tmp;
	uint32_t offset;
	uint64_t value;
	uint64_t attr;
	size_t map_size;

	base = ALIGN(base, LEVEL2_ENTRY_MAP_SIZE);
	tmp = BALIGN(base + size, LEVEL2_ENTRY_MAP_SIZE);
	size = tmp - base;

	attr = get_tt_description(type, LEVEL1_DESCRIPTION_TYPE);

	while (size > 0) {
		offset = base >> LEVEL1_OFFSET;
		value = *(tbase + offset);
		if (value == 0) {
			tmp = (phy_addr_t)vmm_alloc_pages(LEVEL2_PAGES);
			if (!tmp)
				return -ENOMEM;

			*(tbase + offset) = attr | \
				((tmp >> LEVEL2_OFFSET) << LEVEL2_OFFSET);
			value = (uint64_t)tmp;
		}

		if (size > (SIZE_32M)) {
			map_size = BALIGN(base, LEVEL1_ENTRY_MAP_SIZE) - base;
			map_size = map_size ? map_size : LEVEL1_ENTRY_MAP_SIZE;
		} else {
			map_size = size;
		}

		mmu_map_level2_pages((phy_addr_t *)value, base,
				base, map_size, type);
		base += map_size;
		size -= map_size;
	}

	return 0;
}

int mmu_map_memory_region_list(phy_addr_t tbase,
		struct list_head *mem_list)
{
	struct list_head *list;
	struct memory_region *region;

	if (!mem_list)
		return -EINVAL;

	if (is_list_empty(mem_list))
		return -EINVAL;

	list_for_each(mem_list, list) {
		region = list_entry(list,
			struct memory_region, mem_region_list);
		/*
		 * TBD to check the aligment of the address
		 */
		mmu_map_mem((phy_addr_t *)tbase, region->mem_base,
				region->size, region->type);
	};
	
	return 0;
}

phy_addr_t mmu_map_vm_memory(struct list_head *mem_list)
{
	/*
	 * return the table base address, this function
	 * is called when init the vm
	 */
	char *page;
	uint32_t page_nr;
	uint32_t offset;
	uint64_t max_size = CONFIG_MAX_PHYSICAL_SIZE;

	if (!mem_list)
		return 0;

	if (is_list_empty(mem_list))
		return 0;

	/*
	 * calculate how many pages the first level
	 * needed
	 */
	if (GRANULE_TYPE == GRANULE_SIZE_4K)
		offset = 30;
	else if (GRANULE_TYPE == GRANULE_SIZE_16K)
		offset = 25;
	else
		offset = 29;

	page_nr = (max_size >> offset) * sizeof(uint64_t);
	page_nr = page_nr >> 12;
	pr_info("%d pages for the first level table\n", page_nr);

	page = vmm_alloc_pages(page_nr);
	if (!page)
		panic("No memory to map vm memory\n");

	memset(page, 0, SIZE_4K * page_nr);
	mmu_map_memory_region_list((phy_addr_t)page, mem_list);

	return (phy_addr_t)page;
}

int el2_stage2_vmsa_init(void)
{
	/*
	 * now just support arm fvp, TBD to support more
	 * platform
	 */
	uint64_t value;
	struct aa64mmfr0 aa64mmfr0;

	value = read_id_aa64mmfr0_el1();
	memcpy(&aa64mmfr0, &value, sizeof(uint64_t));
	pr_debug("aa64mmfr0: pa_range:0x%x t_gran_16k:0x%x \
			t_gran_64k:0x%x t_gran_4k:0x%x\n",
			aa64mmfr0.pa_range, aa64mmfr0.t_gran_16k,
			aa64mmfr0.t_gran_64k, aa64mmfr0.t_gran_4k);
	return 0;
}
