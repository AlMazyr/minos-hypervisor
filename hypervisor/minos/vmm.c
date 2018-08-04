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
#include <minos/virt.h>
#include <minos/vm.h>
#include <minos/vcpu.h>
#include <minos/mmu.h>

extern unsigned char __el2_ttb0_pgd;
extern unsigned char __el2_ttb0_pud;
extern unsigned char __el2_ttb0_pmd_code;
extern unsigned char __el2_ttb0_pmd_io;

static struct mm_struct host_mm;

LIST_HEAD(mem_list);
static LIST_HEAD(shared_mem_list);

static unsigned long *vm0_mmap_base;

int register_memory_region(struct memtag *res)
{
	struct memory_region *region;

	if (res == NULL)
		return -EINVAL;

	if (!res->enable)
		return -EAGAIN;

	/* only parse the memory for guest */
	if (res->vmid == VMID_HOST)
		return -EINVAL;

	region = (struct memory_region *)
		malloc(sizeof(struct memory_region));
	if (!region) {
		pr_error("No memory for new memory region\n");
		return -ENOMEM;
	}

	pr_info("MEM : 0x%x 0x%x %d %d %s\n", res->mem_base,
			res->mem_end, res->type,
			res->vmid, res->name);

	memset(region, 0, sizeof(struct memory_region));

	region->phy_base = res->mem_base;
	region->size = res->mem_end - res->mem_base + 1;
	region->vmid = res->vmid;

	init_list(&region->list);

	/*
	 * shared memory is for all vm to ipc purpose
	 */
	if (res->type == MEM_TYPE_SHARED) {
		region->type = MEM_TYPE_NORMAL;
		list_add(&shared_mem_list, &region->list);
	} else {
		region->type = res->type;
		list_add_tail(&mem_list, &region->list);
	}

	if (region->type == MEM_TYPE_IO)
		region->vir_base = res->mem_base -
				CONFIG_PLATFORM_IO_BASE + GUEST_IO_MEM_START;
	else
		region->vir_base = res->mem_base -
			CONFIG_PLATFORM_DRAM_BASE + GUEST_NORMAL_MEM_START;

	return 0;
}

static unsigned long alloc_pgd(void)
{
	/*
	 * return the table base address, this function
	 * is called when init the vm
	 *
	 * 2 pages for each VM to map 1T IPA memory
	 *
	 */
	void *page;

	page = __get_free_pages(GUEST_PGD_PAGE_NR,
			GUEST_PGD_PAGE_ALIGN);
	if (!page)
		panic("No memory to map vm memory\n");

	memset(page, 0, SIZE_4K * GUEST_PGD_PAGE_NR);
	return (unsigned long)page;
}

int create_host_mapping(unsigned long vir, unsigned long phy,
		size_t size, unsigned long flags)
{
	unsigned long vir_base, phy_base, tmp;

	/*
	 * for host mapping, IO and Normal memory all mapped
	 * as MEM_BLOCK_SIZE ALIGN
	 */
	vir_base = ALIGN(vir, MEM_BLOCK_SIZE);
	phy_base = ALIGN(phy, MEM_BLOCK_SIZE);
	tmp = BALIGN(vir_base + size, MEM_BLOCK_SIZE);
	size = tmp - vir_base;
	flags |= VM_HOST;

	return create_mem_mapping(&host_mm,
			vir_base, phy_base, size, flags);
}

int destroy_host_mapping(unsigned long vir, size_t size)
{
	unsigned long end;

	end = vir + size;
	end = BALIGN(end, PAGE_SIZE);
	vir = ALIGN(vir, PAGE_SIZE);
	size = end - vir;

	return destroy_mem_mapping(&host_mm, vir, size, VM_HOST);
}

static int create_guest_mapping(struct vm *vm, unsigned long vir,
		unsigned long phy, size_t size, unsigned long flags)
{
	unsigned long vir_base, phy_base, tmp;

	vir_base = ALIGN(vir, SIZE_4K);
	phy_base = ALIGN(phy, SIZE_4K);
	tmp = BALIGN(vir_base + size, SIZE_4K);
	size = tmp - vir_base;

	return create_mem_mapping(&vm->mm, vir_base,
			phy_base, size, flags);
}

static int __used destroy_guest_mapping(struct vm *vm,
		unsigned long vir, size_t size)
{
	unsigned long end;

	end = vir + size;
	end = BALIGN(end, PAGE_SIZE);
	vir = ALIGN(vir, PAGE_SIZE);
	size = end - vir;

	return destroy_mem_mapping(&vm->mm, vir, size, 0);
}

void release_vm_memory(struct vm *vm)
{
	struct mem_block *block, *n;
	struct mm_struct *mm;
	struct page *page, *tmp;

	if (!vm)
		return;

	mm = &vm->mm;
	page = mm->head;

	/*
	 * - release the block list
	 * - release the page table page and page table
	 * - set all the mm_struct to 0
	 * this function will not be called when vm is
	 * running, do not to require the lock
	 */
	list_for_each_entry_safe(block, n, &mm->block_list, list)
		release_mem_block(block);

	while (page != NULL) {
		tmp = page->next;
		release_pages(page);
		page = tmp;
	}

	free_pages((void *)mm->pgd_base);
	memset(mm, 0, sizeof(struct mm_struct));
}

unsigned long get_vm_mmap_info(int vmid, unsigned long *size)
{
	unsigned long vir;

	vir = VM0_MMAP_MEM_START + (vmid * VM_MMAP_MAX_SIZE);
	*size = VM_MMAP_MAX_SIZE;

	return vir;
}

int vm_mmap(struct vm *vm, unsigned long o, unsigned long s)
{
	unsigned long vir;
	unsigned long *vm_pmd;
	struct mm_struct *mm = &vm->mm;
	int start = 0, i, off, count, left;
	unsigned long offset = o, value;
	unsigned long size = s;
	uint64_t attr;

	offset = ALIGN(offset, MEM_BLOCK_SIZE);
	size = BALIGN(size, MEM_BLOCK_SIZE);
	vir = mm->mem_base + offset;

	if (size > VM_MMAP_MAX_SIZE)
		size = VM_MMAP_MAX_SIZE;

	left = size >> MEM_BLOCK_SHIFT;
	start = VM_MMAP_ENTRY_COUNT * vm->vmid;

	/* clear the previous map */
	memset(vm0_mmap_base + start, 0, VM_MMAP_ENTRY_COUNT);

	/*
	 * map the memory as a IO memory in guest to
	 * avoid the cache issue
	 */
	attr = page_table_description(VM_IO | VM_DES_BLOCK);

	while (left > 0) {
		vm_pmd = (unsigned long *)get_mapping_pud(mm->pgd_base, vir, 0);
		if (mapping_error(vm_pmd))
			return -EIO;

		off = (vir - ALIGN(vir, PUD_MAP_SIZE)) >> MEM_BLOCK_SHIFT;
		count = (PUD_MAP_SIZE >> MEM_BLOCK_SHIFT) - off;
	
		for (i = 0; i < count; i++) {
			value = *(vm_pmd + off + i);
			value &= PAGETABLE_ATTR_MASK;
			value |= attr;
			*(vm0_mmap_base + start + i) = value;
		}

		left -= count;
		vir += count << MEM_BLOCK_SHIFT;
	}

	flush_dcache_range((unsigned long)(vm0_mmap_base + start),
				sizeof(unsigned long) * count);
	flush_local_tlb_guest();

	return 0;
}

void vm_unmmap(struct vm *vm)
{
	int offset, i;

	offset = VM_MMAP_ENTRY_COUNT * vm->vmid;

	for (i = 0; i < VM_MMAP_ENTRY_COUNT; i++)
		*(vm0_mmap_base + offset + i) = 0;

	flush_dcache_range((unsigned long)(vm0_mmap_base + offset),
			sizeof(unsigned long) * VM_MMAP_ENTRY_COUNT);
	flush_local_tlb_guest();
}

int alloc_vm_memory(struct vm *vm, unsigned long start, size_t size)
{
	int i, count;
	unsigned long base;
	struct mm_struct *mm = &vm->mm;
	struct mem_block *block;
	struct memory_region *region;

	base = ALIGN(start, MEM_BLOCK_SIZE);
	if (base != start)
		pr_warn("memory base is not mem_block align\n");

	/*
	 * first allocate the page table for vm, since
	 * the vm is not running, do not need to get
	 * the spin lock
	 */
	mm->pgd_base = alloc_pgd();
	if (!mm->pgd_base)
		return -ENOMEM;

	mm->mem_base = base;
	mm->mem_size = size;
	mm->mem_free = size;
	count = size >> MEM_BLOCK_SHIFT;

	/*
	 * here get all the memory block for the vm
	 * TBD: get contiueous memory or not contiueous ?
	 */
	for (i = 0; i < count; i++) {
		block = alloc_mem_block(GFB_VM);
		if (!block)
			goto free_vm_memory;

		block->vmid = vm->vmid;
		list_add_tail(&mm->block_list, &block->list);
		mm->mem_free -= MEM_BLOCK_SIZE;
	}

	/*
	 * begin to map the memory for guest, actually
	 * this is map the ipa to pa in stage 2
	 */
	list_for_each_entry(block, &mm->block_list, list) {
		i = create_guest_mapping(vm, base, block->phy_base,
				MEM_BLOCK_SIZE, VM_NORMAL);
		if (i)
			goto free_vm_memory;

		base += MEM_BLOCK_SIZE;
	}

	/* any hardware IO space for this space */
	list_for_each_entry(region, &mem_list, list) {
		if ((region->vmid != vm->vmid) ||
				(region->type != MEM_TYPE_IO))
			continue;

		pr_info("mapping 0x%x to vm-%d\n",
				region->phy_base, vm->vmid);

		create_guest_mapping(vm, region->phy_base, region->vir_base,
				region->size, region->type);
	}

	return 0;

free_vm_memory:
	release_vm_memory(vm);

	return -ENOMEM;
}

void vm_mm_struct_init(struct vm *vm)
{
	struct mm_struct *mm = &vm->mm;

	init_list(&mm->mem_list);
	init_list(&mm->block_list);
	mm->head = NULL;
	mm->pgd_base = 0;
	spin_lock_init(&mm->lock);
}

int vm_mm_init(struct vm *vm)
{
	struct memory_region *region;
	struct mm_struct *mm = &vm->mm;

	vm_mm_struct_init(vm);

	mm->pgd_base = alloc_pgd();
	if (mm->pgd_base == 0) {
		pr_error("No memory for vm page table\n");
		return -ENOMEM;
	}

	list_for_each_entry(region, &mem_list, list) {
		if (region->vmid != vm->vmid)
			continue;

		create_guest_mapping(vm, region->vir_base, region->phy_base,
				region->size, region->type);
	}

	list_for_each_entry(region, &shared_mem_list, list) {
		create_guest_mapping(vm, region->vir_base, region->phy_base,
				region->size, region->type);
	}

	return 0;
}

int vmm_init(void)
{
	struct vm *vm;
	struct mm_struct *mm;
	struct memory_region *region;
	unsigned long pud;
	unsigned long *tbase;

	/* map all normal memory to host memory space */
	list_for_each_entry(region, &mem_list, list) {
		if (region->type != MEM_TYPE_NORMAL)
			continue;

		if (region->vmid == VMID_HOST)
			continue;

		create_host_mapping(region->phy_base, region->phy_base,
				region->size, region->type);
	}

	/*
	 * 0xc0000000 - 0xffffffff of vm0 (1G space) is used
	 * to mmap the other vm's memory to vm0, 1G space spilt
	 * n region, one vm has a region, so if the system has
	 * max 64 vms, then each vm can mmap 16M max one time
	 */
	vm = get_vm_by_id(0);
	if (!vm)
		panic("no vm found for vmid 0\n");

	mm = &vm->mm;
	pud = get_mapping_pud(mm->pgd_base, VM0_MMAP_MEM_START, 0);
	if (pud > INVALID_MAPPING)
		panic("mmap region should not mapped for vm0\n");

	pud = (unsigned long)get_free_page();
	if (!pud)
		panic("no more memory\n");

	memset((void *)pud, 0, PAGE_SIZE);
	tbase = (unsigned long *)mm->pgd_base;
	create_pud_mapping((unsigned long)tbase,
			VM0_MMAP_MEM_START,
			pud, VM_DES_TABLE | VM_IO | VM_HOST);

	vm0_mmap_base = (unsigned long *)pud;

	return 0;
}

static int vmm_early_init(void)
{
	memset(&host_mm, 0, sizeof(struct mm_struct));
	spin_lock_init(&host_mm.lock);
	host_mm.pgd_base = (unsigned long)&__el2_ttb0_pgd;

	return 0;
}

early_initcall(vmm_early_init);
