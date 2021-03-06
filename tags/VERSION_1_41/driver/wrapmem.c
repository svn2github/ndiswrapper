/*
 *  Copyright (C) 2006 Giridhar Pemmasani
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 */

#define _WRAPMEM_C_

#include "ntoskernel.h"

struct slack_alloc_info {
	struct nt_list list;
	size_t size;
};

static struct nt_list allocs;
static struct nt_list slack_allocs;
static NT_SPIN_LOCK alloc_lock;

#if defined(ALLOC_DEBUG)
struct alloc_info {
	enum alloc_type type;
	size_t size;
#if ALLOC_DEBUG > 1
	struct nt_list list;
	const char *file;
	int line;
#if ALLOC_DEBUG > 2
	ULONG tag;
#endif
#endif
};

static atomic_t alloc_sizes[ALLOC_TYPE_MAX];
#endif

int wrapmem_init(void)
{
	InitializeListHead(&allocs);
	InitializeListHead(&slack_allocs);
	nt_spin_lock_init(&alloc_lock);
	return 0;
}

void wrapmem_exit(void)
{
	enum alloc_type type;
	struct nt_list *ent;
	KIRQL irql;

	/* free all pointers on the slack list */
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	while ((ent = RemoveHeadList(&slack_allocs))) {
		struct slack_alloc_info *info;
		info = container_of(ent, struct slack_alloc_info, list);
#ifdef ALLOC_DEBUG
		atomic_sub(info->size, &alloc_sizes[ALLOC_TYPE_SLACK]);
#endif
		kfree(info);
	}
	nt_spin_unlock_irql(&alloc_lock, irql);
	type = 0;
#ifdef ALLOC_DEBUG
	for (type = 0; type < ALLOC_TYPE_MAX; type++) {
		int n = atomic_read(&alloc_sizes[type]);
		if (n)
			WARNING("%d bytes of memory in %d leaking", n, type);
	}

#if ALLOC_DEBUG > 1
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	while ((ent = RemoveHeadList(&allocs))) {
		struct alloc_info *info;
		info = container_of(ent, struct alloc_info, list);
		atomic_sub(info->size, &alloc_sizes[ALLOC_TYPE_SLACK]);
		WARNING("%p in %d of size %zu allocated at %s(%d) "
#if ALLOC_DEBUG > 2
			"with tag 0x%08X "
#endif
			"leaking; freeing it now", info + 1, info->type,
			info->size, info->file, info->line
#if ALLOC_DEBUG > 2
			, info->tag
#endif
			);
		if (info->type == ALLOC_TYPE_ATOMIC ||
		    info->type == ALLOC_TYPE_NON_ATOMIC)
			kfree(info);
		else if (info->type == ALLOC_TYPE_VMALLOC)
			vfree(info);
		else
			WARNING("invalid type: %d; not freed", info->type);

	}
	nt_spin_unlock_irql(&alloc_lock, irql);
#endif
#endif
	return;
}

void wrapmem_info(void)
{
#ifdef ALLOC_DEBUG
	enum alloc_type type;
	for (type = 0; type < ALLOC_TYPE_MAX; type++)
		INFO("total size of allocations in %d: %d",
		       type, atomic_read(&alloc_sizes[type]));
#endif
}

/* allocate memory and add it to list of allocated pointers; if a
 * driver doesn't free this memory for any reason (buggy driver or we
 * allocate space behind driver's back since we need more space than
 * corresponding Windows structure provides etc.), this gets freed
 * automatically when module is unloaded
 */
void *slack_kmalloc(size_t size)
{
	struct slack_alloc_info *info;
	unsigned int flags, n;
	void *ptr;
	KIRQL irql;

	ENTER4("size = %lu", (unsigned long)size);

	if (current_irql() < DISPATCH_LEVEL)
		flags = GFP_KERNEL;
	else
		flags = GFP_ATOMIC;
	n = size + sizeof(*info);
	info = kmalloc(n, flags);
	if (!info)
		return NULL;
	info->size = size;
	ptr = info + 1;
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	InsertTailList(&slack_allocs, &info->list);
	nt_spin_unlock_irql(&alloc_lock, irql);
#ifdef ALLOC_DEBUG
	atomic_add(size, &alloc_sizes[ALLOC_TYPE_SLACK]);
#endif
	TRACE4("%p, %p", info, ptr);
	EXIT4(return ptr);
}

/* free pointer and remove from list of allocated pointers */
void slack_kfree(void *ptr)
{
	struct slack_alloc_info *info;
	KIRQL irql;

	ENTER4("%p", ptr);
	info = ptr - sizeof(*info);
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	RemoveEntryList(&info->list);
	nt_spin_unlock_irql(&alloc_lock, irql);
#ifdef ALLOC_DEBUG
	atomic_sub(info->size, &alloc_sizes[ALLOC_TYPE_SLACK]);
#endif
	kfree(info);
	EXIT4(return);
}

#if defined(ALLOC_DEBUG)
void *wrap_kmalloc(size_t size, unsigned flags, const char *file, int line)
{
	struct alloc_info *info;
#if ALLOC_DEBUG > 1
	KIRQL irql;
#endif

	info = kmalloc(size + sizeof(*info), flags);
	if (!info)
		return NULL;
	if (flags & GFP_ATOMIC)
		info->type = ALLOC_TYPE_ATOMIC;
	else
		info->type = ALLOC_TYPE_NON_ATOMIC;
	info->size = size;
	atomic_add(size, &alloc_sizes[info->type]);
#if ALLOC_DEBUG > 1
	info->file = file;
	info->line = line;
#if ALLOC_DEBUG > 2
	info->tag = 0;
#endif
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	InsertTailList(&allocs, &info->list);
	nt_spin_unlock_irql(&alloc_lock, irql);
#endif
	return (info + 1);
}

void wrap_kfree(void *ptr)
{
	struct alloc_info *info;
#if ALLOC_DEBUG > 1
	KIRQL irql;
#endif

	info = ptr - sizeof(*info);
	atomic_sub(info->size, &alloc_sizes[info->type]);
#if ALLOC_DEBUG > 1
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	RemoveEntryList(&info->list);
	nt_spin_unlock_irql(&alloc_lock, irql);
	if (!(info->type == ALLOC_TYPE_ATOMIC ||
	      info->type == ALLOC_TYPE_NON_ATOMIC))
		WARNING("invliad type: %d", info->type);
#endif
	kfree(info);
}

void *wrap_vmalloc(unsigned long size, const char *file, int line)
{
	struct alloc_info *info;
#if ALLOC_DEBUG > 1
	KIRQL irql;
#endif

	info = vmalloc(size + sizeof(*info));
	if (!info)
		return NULL;
	info->type = ALLOC_TYPE_VMALLOC;
	info->size = size;
	atomic_add(size, &alloc_sizes[info->type]);
#if ALLOC_DEBUG > 1
	info->file = file;
	info->line = line;
#if ALLOC_DEBUG > 2
	info->tag = 0;
#endif
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	InsertTailList(&allocs, &info->list);
	nt_spin_unlock_irql(&alloc_lock, irql);
#endif
	return (info + 1);
}

void *wrap__vmalloc(unsigned long size, unsigned int gfp_mask, pgprot_t prot,
		    const char *file, int line)
{
	struct alloc_info *info;
#if ALLOC_DEBUG > 1
	KIRQL irql;
#endif

	info = __vmalloc(size + sizeof(*info), gfp_mask, prot);
	if (!info)
		return NULL;
	info->type = ALLOC_TYPE_VMALLOC;
	info->size = size;
	atomic_add(size, &alloc_sizes[info->type]);
#if ALLOC_DEBUG > 1
	info->file = file;
	info->line = line;
#if ALLOC_DEBUG > 2
	info->tag = 0;
#endif
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	InsertTailList(&allocs, &info->list);
	nt_spin_unlock_irql(&alloc_lock, irql);
#endif
	return (info + 1);
}

void wrap_vfree(void *ptr)
{
	struct alloc_info *info;
#if ALLOC_DEBUG > 1
	KIRQL irql;
#endif

	info = ptr - sizeof(*info);
	atomic_sub(info->size, &alloc_sizes[info->type]);
#if ALLOC_DEBUG > 1
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	RemoveEntryList(&info->list);
	nt_spin_unlock_irql(&alloc_lock, irql);
	if (info->type != ALLOC_TYPE_VMALLOC)
		WARNING("invliad type: %d", info->type);
#endif
	vfree(info);
}

void *wrap_alloc_pages(unsigned flags, unsigned int size,
		       const char *file, int line)
{
	struct alloc_info *info;
#if ALLOC_DEBUG > 1
	KIRQL irql;
#endif

	size += sizeof(*info);
	info = (struct alloc_info *)__get_free_pages(flags, get_order(size));
	if (!info)
		return NULL;
	info->type = ALLOC_TYPE_PAGES;
	info->size = size;
	atomic_add(size, &alloc_sizes[info->type]);
#if ALLOC_DEBUG > 1
	info->file = file;
	info->line = line;
#if ALLOC_DEBUG > 2
	info->tag = 0;
#endif
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	InsertTailList(&allocs, &info->list);
	nt_spin_unlock_irql(&alloc_lock, irql);
#endif
	return info + 1;
}

void wrap_free_pages(unsigned long ptr, int order)
{
	struct alloc_info *info;
#if ALLOC_DEBUG > 1
	KIRQL irql;
#endif

	info = (void *)ptr - sizeof(*info);
	atomic_sub(info->size, &alloc_sizes[info->type]);
#if ALLOC_DEBUG > 1
	irql = nt_spin_lock_irql(&alloc_lock, DISPATCH_LEVEL);
	RemoveEntryList(&info->list);
	nt_spin_unlock_irql(&alloc_lock, irql);
	if (info->type != ALLOC_TYPE_PAGES)
		WARNING("invliad type: %d", info->type);
#endif
	free_pages((unsigned long)info, get_order(info->size));
}

#if ALLOC_DEBUG > 1
#undef ExAllocatePoolWithTag
void *wrap_ExAllocatePoolWithTag(enum pool_type pool_type, SIZE_T size,
				 ULONG tag, const char *file, int line)
{
	void *addr;
	struct alloc_info *info;

	ENTER4("pool_type: %d, size: %lu, tag: %u", pool_type, size, tag);
	addr = ExAllocatePoolWithTag(pool_type, size, tag);
	if (addr) {
		info = addr - sizeof(unsigned long) - sizeof(*info);
		info->file = file;
		info->line = line;
#if ALLOC_DEBUG > 2
		info->tag = tag;
#endif
	}
	EXIT4(return addr);
}
#endif

int alloc_size(enum alloc_type type)
{
	if (type >= 0 && type < ALLOC_TYPE_MAX)
		return atomic_read(&alloc_sizes[type]);
	else
		return -EINVAL;
}

#endif // ALLOC_DEBUG
