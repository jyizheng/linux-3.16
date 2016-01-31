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
#define MM_REGION_PAGE_MASK	0xfffffffe

/* bit 31 and 30 is used for zone, see mm.h */
#define MM_PAGE_ZONE_MASK	0xc0000000

static inline bool is_region_free_page(struct page *page)
{
	unsigned long reg = (unsigned long)page->reg;

	return (reg & MM_REGION_PAGE_FLAG) ? true : false;
}

static inline void unmap_file_region(struct mm_region *reg)
{
	struct page *head = reg->head;
	struct page *page;
	struct address_space *mapping;
	int ret = -1;
	int i;

	for (i = 0; i < reg->size; i++) {
		page = head  + i;

		if (!trylock_page(page))
			continue;

		/* For used page in the region, unmap it */
		if (!is_region_free_page(page)) {
			mapping = page_mapping(page);
			if (page_mapped(page) && mapping) {
				ret = try_to_unmap(page, TTU_UNMAP);
				pr_info("unmaped page in region:%lx, ret:%d\n",
						page_to_pfn(page), ret);
			}
			if (ret == SWAP_SUCCESS) {
				__clear_page_locked(page);
				continue;
			}
		}
		unlock_page(page);
	}
}

static void print_mm_region_free(struct mm_region *reg)
{
	pr_info("freesize:%d\n", reg->freesize);
	pr_info("index:%d\n", reg->index);

	if (reg->index == reg->freesize)
		free_mm_region(reg);
	else 
		unmap_file_region(reg);
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
	page->reg = (struct mm_region *)
			(MM_REGION_PAGE_FLAG | (unsigned long)page->reg);

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

	if (page != NULL)
		page->reg = (struct mm_region *)
			    (MM_REGION_PAGE_MASK & (unsigned long)page->reg);
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
	pr_info("task is:%s, oom_score_min:%d, flag:%x, ppid:%d, pid:%d\n",
			current->comm,
			current->signal->oom_score_adj_min,
			current->flags, current->parent->pid,
			current->pid);
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

/* Hijack page cache allocation */
struct page *__page_cache_alloc_mm_opt(gfp_t gfp_mask,
			struct address_space *x)
{
	struct mm_domain *dom;
	struct page *page;
	gfp_t old_gfp_mask = gfp_mask;

	if (x == NULL)
		goto normal;
	if (!S_ISREG(x->host->i_mode))
		goto normal;

	pr_info("task is:%s\n", current->comm);
	pr_info("alloc for inode %ld\n", x->host->i_ino);

	dom = x->file_domain;
	gfp_mask |= __GFP_FILE_CACHE;

	if (x->flags & AS_READONLY) {
		gfp_mask |= __GFP_READONLY;
	}
	
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
	return alloc_pages(old_gfp_mask, 0);
}

struct page *alloc_page_vmalloc(gfp_t gfp)
{
	return alloc_page(gfp);
}

struct page *alloc_pages_node_vmalloc(int node,
			gfp_t gfp, int order)
{
	return alloc_pages_node(node, gfp, order);
}

/* Bank extent rb_tree manipulation */
struct bank_extent *bank_extent_search(struct rb_root *root,
		unsigned long pfn, unsigned int size)
{
  	struct rb_node *node = root->rb_node;

  	while (node) {
  		struct bank_extent *data = container_of(node,
					struct bank_extent, ext_rb);

		if (pfn < data->start_pfn)
  			node = node->rb_left;
		else if (pfn > data->start_pfn + data->nr_pages)
  			node = node->rb_right;
		else {
			BUG_ON(pfn + size > data->start_pfn + data->nr_pages);
  			return data;
		}
	}
	return NULL;
}

int bank_extent_insert(struct rb_root *root, struct bank_extent *ext)
{
  	struct rb_node **new = &(root->rb_node), *parent = NULL;

  	/* Figure out where to put new node */
  	while (*new) {
		struct bank_extent *this = container_of(*new,
					struct bank_extent, ext_rb);
		int result = ext->start_pfn - this->start_pfn;

		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else
			return 0;
	}

  	/* Add new node and rebalance tree. */
  	rb_link_node(&ext->ext_rb, parent, new);
  	rb_insert_color(&ext->ext_rb, root);

	return 1;
}

static void migrate_extent(struct bank_extent *ext)
{
	struct mm_region *reg;
	unsigned long pfn = ext->start_pfn;
	int i;

	for (i = 0; i < ext->nr_pages; i++) {
		struct page *page = pfn_to_page(pfn);

		reg = (struct mm_region *)
			(MM_REGION_PAGE_MASK & (unsigned long)page->reg);
		if (page->reg != NULL)
			print_mm_region_free(reg);
	}
}

static void bank_extent_traverse(struct rb_root *root)
{
	struct rb_node *node;

	for (node = rb_first(root); node; node = rb_next(node)) {
		struct bank_extent *ext = rb_entry(node, struct bank_extent, ext_rb);

		pr_info("extent is pfn=%lx, size=%ld\n", ext->start_pfn, ext->nr_pages);
		migrate_extent(ext);
	}
}

int sysctl_compact_vm;
int sysctl_compact_file;

static void bank_rb_init(struct bank *bank, unsigned int start_pfn)
{
	struct bank_extent *ext;
	if (bank->ext_rb.rb_node == NULL) {
		ext = kmalloc(sizeof(struct bank_extent), GFP_KERNEL);
		if (ext) {
			ext->start_pfn = start_pfn;
			ext->nr_pages = 1 << (MAX_ORDER - 1);
			bank_extent_insert(&bank->ext_rb, ext);
		}
	}
}

static void compact_one_bank(struct bank *bank)
{
	struct bank_extent *ext;
	struct bank_extent *new_ext;
	struct page *page;
	int i, j;
	int t = MIGRATE_MOVABLE;
	struct list_head *list;

	for (i = 0; i < MAX_ORDER; i++) {
		pr_info("nr_free:%lx\n", bank->free_area[i].nr_free);
	}

	for (i = 0; i < MAX_ORDER; i++) {
		list = &bank->free_area[i].free_list[t];
		if (bank->free_area[i].nr_free == 0)
			continue;

		j = 0;

		list_for_each_entry(page, list, lru) {
			unsigned long pfn = page_to_pfn(page);
			unsigned int nr_pages = 1 << i;
			
			pr_info("pfn:%lx, order:%d\n", pfn, i);

			ext = bank_extent_search(&bank->ext_rb, pfn, nr_pages);

			pr_info("pfn:%lx, nr_pages:%lx\n", ext->start_pfn, ext->nr_pages);

			if (ext->start_pfn == pfn) {
				ext->start_pfn = pfn + nr_pages;
				ext->nr_pages -= nr_pages;
			} else if (ext->start_pfn + ext->nr_pages == pfn + nr_pages) {
				ext->nr_pages -= nr_pages;
			} else {
				int new_nr_pages = pfn - ext->start_pfn;

				new_ext = kmalloc(sizeof(struct bank_extent), GFP_KERNEL);
				if (!new_ext)
					goto out;
				new_ext->start_pfn = pfn + nr_pages;
				new_ext->nr_pages = ext->nr_pages - new_nr_pages - nr_pages;
				ext->nr_pages = new_nr_pages;

				pr_info("insert new extent\n");
				bank_extent_insert(&bank->ext_rb, new_ext);
			}

			if (++j == bank->free_area[i].nr_free)
				break;
		}
	}
	bank_extent_traverse(&bank->ext_rb);
out:
	return;
}

static void compact_vm_bank(struct zone *zone)
{
	struct bank bank;
	int i;
	
	for (i = 0; i < zone->nr_vm_bank; i++) {
		bank = zone->free_bank_vm[i];
		spin_lock(&bank.lock);
		bank_rb_init(&bank, zone->vm_start_pfn[i]);

		pr_info("pfn:%x\n", zone->vm_start_pfn[i]);
		compact_one_bank(&bank);
		spin_unlock(&bank.lock);
	}
}

static void compact_normal_vm_bank(void)
{
	struct zone *zone;
	
	for_each_zone(zone) {
		if (strcmp(zone->name, "Normal") == 0)
			compact_vm_bank(zone);
	}
}

static void compact_highmem_vm_bank(void)
{
	struct zone *zone;
	
	for_each_zone(zone) {
		if (strcmp(zone->name, "HighMem") == 0)
			compact_vm_bank(zone);
	}
}


static void compact_file_bank(struct zone *zone)
{
	struct bank bank;
	int i;
	
	for (i = 0; i < zone->nr_file_bank; i++) {
		bank = zone->free_bank_file[i];
		spin_lock(&bank.lock);
		bank_rb_init(&bank, zone->file_start_pfn[i]);

		pr_info("pfn:%x\n", zone->file_start_pfn[i]);
		compact_one_bank(&bank);
		spin_unlock(&bank.lock);
	}
}

static void compact_normal_file_bank(void)
{
	struct zone *zone;
	
	for_each_zone(zone) {
		if (strcmp(zone->name, "Normal") == 0)
			compact_file_bank(zone);
	}
}

static void compact_highmem_file_bank(void)
{
	struct zone *zone;
	
	for_each_zone(zone) {
		if (strcmp(zone->name, "HighMem") == 0)
			compact_file_bank(zone);
	}
}

int compact_vm_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (ret)
		return ret;
	if (write) {
		if (sysctl_compact_vm & 1)
			compact_normal_vm_bank();
		if (sysctl_compact_vm & 2)
			compact_highmem_vm_bank();
	}
	return 0;
}

int compact_file_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (ret)
		return ret;
	if (write) {
		if (sysctl_compact_file & 1)
			compact_normal_file_bank();
		if (sysctl_compact_file & 2)
			compact_highmem_file_bank();
	}
	return 0;
}

