// SPDX-License-Identifier: GPL-2.0-only
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/sched.h>
#include <linux/ept.h>
#include <linux/mm_types.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/hugetlb.h>

#define EPT_BITS 39ULL
#define EPT_SIZE (1ULL << EPT_BITS)

static bool ept_walk_to_pte_table(struct mm_struct *mm, unsigned long target_vaddr,
								unsigned long *out_pfn)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	struct page *pte_page;

	pgd = pgd_offset(mm, target_vaddr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return false;

	p4d = p4d_offset(pgd, target_vaddr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return false;

	pud = pud_offset(p4d, target_vaddr);
	if (pud_none(*pud) || pud_bad(*pud))
		return false;

	if (pud_trans_huge(*pud) || pud_devmap(*pud))
		return false;

	pmd = pmd_offset(pud, target_vaddr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return false;

	if (pmd_trans_huge(*pmd) || pmd_devmap(*pmd))
		return false;

	/*
	 * arm64 discrepancy
	 * Do NOT use  pmd_pfn() here.
	 */
	pte_page = pmd_page(*pmd);
	if (!pte_page)
		return false;

	*out_pfn = page_to_pfn(pte_page);
	return true;
}

/* Custom fault handler */
static vm_fault_t ept_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct mm_struct *mm = vma->vm_mm;

	unsigned long fault_page = vmf->address & PAGE_MASK;
	unsigned long target_vaddr = vmf->pgoff << PMD_SHIFT;
	unsigned long pfn;

	if (!ept_walk_to_pte_table(mm, target_vaddr, &pfn)) {
		/* oh noes, no mapping; use zero page */
		pfn = my_zero_pfn(fault_page);
	}

	return vmf_insert_pfn(vma, fault_page, pfn);
}

static const struct vm_operations_struct ept_vm_ops = {
	.fault = ept_fault,
};

void ept_invalidate(struct mm_struct *mm, unsigned long address)
{
	struct vm_area_struct *vma;
	unsigned long required_pgoff = address >> PMD_SHIFT;

	VMA_ITERATOR(vmi, mm, 0);

	/* why even keep a list in mm_struct, just walk all vmas */
	for_each_vma(vmi, vma) {
		unsigned long nr_pages;
		unsigned long vma_off_pages;
		unsigned long ept_addr;

		if (vma->vm_ops != &ept_vm_ops)
			continue;

		nr_pages = vma_pages(vma);
		if (required_pgoff < vma->vm_pgoff ||
		    required_pgoff >= vma->vm_pgoff + nr_pages)
			continue;

		vma_off_pages = required_pgoff - vma->vm_pgoff;
		ept_addr = vma->vm_start + (vma_off_pages << PAGE_SHIFT);

		zap_vma_ptes(vma, ept_addr, PAGE_SIZE);
		// flush_tlb_range(vma, ept_addr, ept_addr + PAGE_SIZE);
		// zap_vma_ptes already flushes tlb
	}
}
EXPORT_SYMBOL_GPL(ept_invalidate);

static int ept_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long off_bytes = vma->vm_pgoff << PAGE_SHIFT;

	/* root&read-only mapping */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (vma->vm_flags & VM_WRITE)
		return -EPERM;

	/* offset + size must fit in the exposed 512GiB space */
	if (!PAGE_ALIGNED(size) || size == 0)
		return -EINVAL;

	if (off_bytes + size > EPT_SIZE)
		return -EINVAL;

	vm_flags_clear(vma, VM_MAYWRITE); /* non cow */
	vma->vm_ops = &ept_vm_ops;
	vm_flags_set(vma, VM_PFNMAP | VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP);

	return 0;
}

static const struct file_operations ept_fops = {
	.owner  = THIS_MODULE,
	.mmap   = ept_mmap,
};

static struct miscdevice ept_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "ept",
	.fops  = &ept_fops,
	.mode  = 0600, /* root-only access */
};

static int __init ept_init(void)
{
	return misc_register(&ept_dev);
}

static void __exit ept_exit(void)
{
	misc_deregister(&ept_dev);
}

module_init(ept_init);
module_exit(ept_exit);
MODULE_LICENSE("GPL");
