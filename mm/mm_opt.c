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

#define MM_OPT_REGION_ORDER	10U
#define MM_PAGE_ZONE_MASK	0xc000000000000000UL

static void prepare_region_free_page(struct page *page)
{
	BUG_ON(page_mapcount(page) != 0);
	BUG_ON(page->mapping != NULL);
	BUG_ON(page_count(page) != 0);

	set_page_count(page, 1);
	page->flags &= MM_PAGE_ZONE_MASK;
	page->index = 0x0;
	arch_free_page(page, 0);
	kernel_map_pages(page, 1, 0);
}

static void prepare_buddy_free_page(struct page *page,
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

static void check_free_region(struct mm_region *reg)
{
	return;
}

static void mm_region_free(struct mm_region *reg)
{
	check_free_region(reg);
	__free_pages(reg->head, MM_OPT_REGION_ORDER);
}

void free_mm_region(struct mm_region *reg)
{
	if (reg == NULL)
		return;

	prepare_buddy_free_page(reg->head, MM_OPT_REGION_ORDER);
	mm_region_free(reg);
	list_del(&reg->domlist);
	if (reg->dom->cache_reg == reg)
		reg->dom->cache_reg = NULL;
	reg->dom->size--;
	reg->head = NULL;
	kfree(reg);
}
EXPORT_SYMBOL(free_mm_region);

int mm_region_free_page(struct page *page)
{
	struct mm_region *reg = page->reg;

	prepare_region_free_page(page);
	list_add(&page->lru, &reg->freelist);
	reg->freesize++;

	if (reg->freesize == reg->size)
		free_mm_region(reg);

	return 0;
}

/************************** Allocate Part **********************/
static void prepare_region_pages(struct page *page, unsigned long order)
{
	int i;

	VM_BUG_ON(page_count(page) != 1);
	set_page_count(page, 1);

	for (i = 1; i < 1 << order; i++) {
		struct page *p = page + i;

		VM_BUG_ON(page_count(p) != 0);
		set_page_count(p, 1);
	}
}

/* Alloc a new region from buddy system */
static struct mm_region *mm_alloc_region(gfp_t gfp_mask, struct mm_domain *dom)
{
	int i;
	struct page *page; 
	struct mm_region *reg;

	reg = (struct mm_region *)
		kmalloc(sizeof(struct mm_region), GFP_KERNEL);
	if (reg == NULL)
		return NULL;

	page = alloc_pages(gfp_mask, MM_OPT_REGION_ORDER);
	if (page == NULL) {
		kfree(reg);
		return NULL;
	}
	
	pr_info("A region is allocated for %s\n", current->comm);

	prepare_region_pages(page, MM_OPT_REGION_ORDER);
	reg->head = page;
	reg->size = 1 << MM_OPT_REGION_ORDER;
	reg->index = 0;
	INIT_LIST_HEAD(&reg->freelist);
	INIT_LIST_HEAD(&reg->domlist);
	reg->freesize = 0;
	reg->dom = dom;	

	for (i = 0; i < reg->size; i++)
		page = &reg->head[i];

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

	if (page == NULL)
		goto out;
	
	arch_alloc_page(page, 0);
	kernel_map_pages(page, 1, 1);

	if (gfp_mask & __GFP_ZERO) {
		void *kaddr;

		kaddr = kmap_atomic(page);
		memset(kaddr, 0, 4096);
		kunmap_atomic(kaddr);
	}
out:
	return page;
}

/* Check whether a region is full */
static bool mm_region_is_full(struct mm_region *reg)
{
	VM_BUG_ON(reg == NULL);
	return  (reg->index == reg->size && reg->freesize == 0);
}

/* Return a region that is not full */
static struct mm_region *mm_domain_find_region(struct mm_domain *dom,
		gfp_t gfp_mask)
{
	struct mm_region *reg;

	VM_BUG_ON(dom == NULL);
	list_for_each_entry(reg, &dom->domlist_head, domlist) {
		if (!mm_region_is_full(reg))
			return reg;
	}

	reg = mm_alloc_region(gfp_mask, dom);
	if (reg != NULL) {
		list_add(&reg->domlist, &dom->domlist_head);
		dom->size++;
		dom->cache_reg = reg;
	}
	return reg;
}

#ifdef CONFIG_MM_OPT_VM
/* Hijack virtual process page allocation */
struct page *alloc_pages_vma_mm_opt(gfp_t gfp_mask, int order,
		struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm;
	struct mm_domain *dom;
	struct page *page;
	gfp_t old_gfp_mask;
	
	VM_BUG_ON(order != 0);
	mm = vma->vm_mm;
	dom = mm->vmdomain;
	old_gfp_mask = gfp_mask;
	gfp_mask |= __GFP_VM_PAGE;

	if (dom->cache_reg == NULL || mm_region_is_full(dom->cache_reg)) {
		struct mm_region *reg;

		reg = mm_domain_find_region(dom, gfp_mask);
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
#endif

#ifdef CONFIG_MM_OPT_FILE
/* Hijack page cache allocation */
struct page *__page_cache_alloc_mm_opt(gfp_t gfp_mask,
		struct address_space *x)
{
	struct mm_domain *dom;
	struct page *page;
	gfp_t old_gfp_mask;

	if (x == NULL)
		goto normal;

	dom = x->file_domain;
	old_gfp_mask = gfp_mask;
	gfp_mask |= __GFP_FILE_CACHE;

	if (x->flags & AS_READONLY)
		gfp_mask |= __GFP_READONLY;
	
	if (dom->cache_reg == NULL || mm_region_is_full(dom->cache_reg)) {
		struct mm_region *reg;

		reg = mm_domain_find_region(dom, gfp_mask);
		if (reg == NULL)
			goto normal;
		dom->cache_reg = reg;
	}

	page = mm_region_alloc_page(dom->cache_reg, gfp_mask);
	if (page == NULL) 
		goto normal;
	return page;
normal:
	return alloc_pages(old_gfp_mask, 0);
}
#endif
