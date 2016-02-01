/*
 *  linux/mm/mm_opt.c
 *
 *  Copyright (C) 2015  Yizheng Jiao
 */

#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/pfn.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/mmzone.h>
#include <linux/compiler.h>
#include <linux/stddef.h>
#include <linux/module.h>
#include <linux/rmap.h>

#include "internal.h"

#define MM_OPT_REGION_ORDER	4U
#define MM_REGION_PAGE_FLAG	0x01
#define MM_REGION_PAGE_MASK	0xfffffffffffffffe

/* bit 31 and 30 is used for zone, see mm.h */
#define MM_PAGE_ZONE_MASK	0xc000000000000000UL

int sysctl_compact_vm;

int compact_vm_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	return 0;
}

static inline bool is_region_free_page(struct page *page)
{
	unsigned long reg = (unsigned long)page->reg;

	return (reg & MM_REGION_PAGE_FLAG) ? true : false;
}

static bool free_pages_prepare_mm_opt(struct page *page)
{
	if (page_mapcount(page) != 0 || page->mapping != NULL
		|| page_count(page) != 0) {
		dump_page(page, __func__);
		BUG();
	}

	set_page_count(page, 1);
	page->flags &= MM_PAGE_ZONE_MASK;
	page->index = 0x0;
	arch_free_page(page, 0);
	kernel_map_pages(page, 1, 0);

	return true;
}

static void prep_compound_page_mm_opt(struct page *page,
		unsigned long order)
{
	int i;
	int nr_pages = 1 << order;

	set_page_count(page, 1);
	page->reg = NULL;
	page->flags &= MM_PAGE_ZONE_MASK;
	page->index = 0x0;
	page->mapping = NULL;

	for (i = 1; i < nr_pages; i++) {
		struct page *p = &page[i];

		p->flags &= MM_PAGE_ZONE_MASK;
		p->index = 0x0;
		p->mapping = NULL;
		set_page_count(p, 0);
		p->reg = NULL;
	}
}

/* This is for sanity check, when eveything 
 * works fine disable it 
 */
static void check_free_region(struct mm_region *reg)
{
	struct page *page;
	unsigned long pfn1 = page_to_pfn(reg->head);
	unsigned long pfn2;
	int flags[1 << MM_OPT_REGION_ORDER];
	int size = 1 << MM_OPT_REGION_ORDER;
	int i = 0;
	int j = 0;

	for (j = 0; j < size; j++)
		flags[j] = 0;

	list_for_each_entry(page, &reg->freelist, lru) {
		pfn2 = page_to_pfn(page);
		BUG_ON(pfn2 < pfn1 || pfn2 > pfn1 + size - 1);
		BUG_ON(flags[pfn2 - pfn1] == 1);
		flags[pfn2 - pfn1] = 1;
		i++;
	}
	BUG_ON(i != reg->index);
}

static void mm_region_free(struct mm_region *reg)
{
	unsigned int order = MM_OPT_REGION_ORDER;

	check_free_region(reg);
	__free_pages(reg->head, order);
}

void free_mm_region(struct mm_region *reg)
{
	if (reg == NULL)
		return;

	prep_compound_page_mm_opt(reg->head, MM_OPT_REGION_ORDER);
	mm_region_free(reg);
	list_del(&reg->domlist);
	if (reg->dom->cache_reg == reg)
		reg->dom->cache_reg = NULL;
	reg->dom->size--;
	reg->head = NULL;
	kfree(reg);
}
EXPORT_SYMBOL(free_mm_region);

static int destroy_compound_page_mm_opt(struct page *page, unsigned long order)
{
	int i;
	int nr_pages = 1 << order;
	
	if (page_count(page) != 1) {
		dump_page(page, __func__);
		BUG();
	}
	set_page_count(page, 1);
	for (i = 1; i < nr_pages; i++) {
		struct page *p = page + i;
		if (page_count(p) != 0) {
			dump_page(p, __func__);
			BUG();
		}
		set_page_count(p, 1);
	}

	return 0;
}

/* Alloc a new region from buddy system */
static struct mm_region *mm_alloc_region(gfp_t gfp_mask, struct mm_domain *dom)
{
	int i;
	int bad;
	struct page *page; 
	struct mm_region *reg = (struct mm_region *)
		kmalloc(sizeof(struct mm_region), GFP_KERNEL);

	if (reg == NULL)
		return NULL;
	page = alloc_pages(gfp_mask, MM_OPT_REGION_ORDER);
	if (page == NULL) {
		kfree(reg);
		return NULL;
	}
	
	bad = destroy_compound_page_mm_opt(page, MM_OPT_REGION_ORDER);
	reg->head = page;
	reg->size = 1 << MM_OPT_REGION_ORDER;
	reg->index = 0;
	INIT_LIST_HEAD(&reg->freelist);
	INIT_LIST_HEAD(&reg->domlist);
	reg->freesize = 0;
	reg->dom = dom;	

	for (i = 0; i < reg->size; i++) {
		page = &reg->head[i];
		page->reg = (struct mm_region *)
			    (MM_REGION_PAGE_MASK & (unsigned long)reg);
	}

	return reg;
}

/* Try to allocate a page from NON-empty region */
static struct page* mm_region_alloc_page(struct mm_region *reg, gfp_t gfp_mask)
{
	struct list_head *freelist = &reg->freelist;
	struct page *page = NULL;
	
	if (reg->freesize > 0) {
		page = list_entry(freelist->next, struct page, lru);
		list_del(&page->lru);
		page->reg = reg;
		reg->freesize--;
	} else if (reg->index < reg->size) {
		page = reg->head + reg->index;
		page->reg = reg;
		reg->index++;
	}

	if (page != NULL) {
		arch_alloc_page(page, 0);
		kernel_map_pages(page, 1, 1);
	}

	if ((gfp_mask & __GFP_ZERO) && page != NULL) {
		void *kaddr = kmap_atomic(page);
		memset(kaddr, 0, 4096);
		kunmap_atomic(kaddr);
	}

	return page;
}

int mm_region_free_page(struct page *page)
{
	struct mm_region *reg = page->reg;

	free_pages_prepare_mm_opt(page);
	list_add(&page->lru, &reg->freelist);
	reg->freesize++;

	if (reg->freesize == reg->size) {
		prep_compound_page_mm_opt(reg->head, MM_OPT_REGION_ORDER);
		mm_region_free(reg);
		list_del(&reg->domlist);
		if (reg->dom->cache_reg == reg)
			reg->dom->cache_reg = NULL;
		reg->dom->size--;
		reg->head = NULL;
		kfree(reg);
	}
	return 0;
}

/* Check whether a region is full */
static bool mm_region_is_full(struct mm_region *reg)
{
	VM_BUG_ON(reg == NULL);
	if (reg->index == reg->size && reg->freesize == 0)
		return true;
	return false;
}

/*
 * Check whether a domain is full. Domain is full 
 * when all regions are full
 */
static bool mm_domain_is_full(struct mm_domain *dom)
{
	struct mm_region *reg;

	VM_BUG_ON(dom == NULL);
	list_for_each_entry(reg, &dom->domlist_head, domlist) {
		if (!mm_region_is_full(reg))
			return false;
	}
	
	return true;
}

/* Return a region that is not full */
static struct mm_region *mm_domain_find_region(struct mm_domain *dom)
{
	struct mm_region *reg;

	VM_BUG_ON(dom == NULL);
	list_for_each_entry(reg, &dom->domlist_head, domlist) {
		if (!mm_region_is_full(reg))
			return reg;
	}

	return NULL;
}

/* Hijack virtual process page allocation */
struct page *alloc_pages_vma_mm_opt(gfp_t gfp_mask, int order,
		struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm;
	struct mm_domain *dom;
	struct page *page;

	gfp_t old_gfp_mask = gfp_mask;
	VM_BUG_ON(order != 0);
	mm = vma->vm_mm;
	dom = mm->vmdomain;
	gfp_mask |= __GFP_VM_PAGE;

	if (dom->size == 0 || mm_domain_is_full(dom)) {
		struct mm_region *reg = mm_alloc_region(gfp_mask, dom);

		if (reg == NULL)
			goto normal;
		list_add(&reg->domlist, &dom->domlist_head);
		dom->size++;
		dom->cache_reg = reg;
	}

	if (dom->cache_reg == NULL || mm_region_is_full(dom->cache_reg)) {
		struct mm_region *reg = mm_domain_find_region(dom);

		if (reg == NULL)
			goto normal;
		dom->cache_reg = reg;
	}

	page = mm_region_alloc_page(dom->cache_reg, gfp_mask);
	if (page == NULL) 
		goto normal;
	return page;
normal:
	page = alloc_pages(old_gfp_mask, order);
	return page;
}
