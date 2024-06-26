/*
 * Copyright (c) 2016 - 2023 Pedro Falcato
 * This file is part of Onyx, and is released under the terms of the MIT License
 * check LICENSE at the root directory for more information
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <onyx/arch.h>
#include <onyx/compiler.h>
#include <onyx/copy.h>
#include <onyx/cpu.h>
#include <onyx/dentry.h>
#include <onyx/dev.h>
#include <onyx/file.h>
#include <onyx/filemap.h>
#include <onyx/gen/trace_vm.h>
#include <onyx/log.h>
#include <onyx/mm/amap.h>
#include <onyx/mm/kasan.h>
#include <onyx/mm/shmem.h>
#include <onyx/mm/slab.h>
#include <onyx/mm/vm_object.h>
#include <onyx/page.h>
#include <onyx/pagecache.h>
#include <onyx/paging.h>
#include <onyx/panic.h>
#include <onyx/percpu.h>
#include <onyx/process.h>
#include <onyx/random.h>
#include <onyx/spinlock.h>
#include <onyx/sysfs.h>
#include <onyx/timer.h>
#include <onyx/user.h>
#include <onyx/utils.h>
#include <onyx/vfs.h>
#include <onyx/vm.h>
#include <onyx/vm_layout.h>

#include <libdict/dict.h>
#include <uapi/fcntl.h>
#include <uapi/memstat.h>

bool is_initialized = false;
static bool enable_aslr = true;

uintptr_t high_half = arch_high_half;
uintptr_t low_half_max = arch_low_half_max;
uintptr_t low_half_min = arch_low_half_min;

/* These addresses are either absolute, or offsets, depending on the architecture.
 * The corresponding arch/ code is responsible for patching these up using
 * vm_update_addresses.
 */
uintptr_t vmalloc_space = arch_vmalloc_off;

void kmalloc_init();
int populate_shared_mapping(void *page, struct file *fd, struct vm_area_struct *entry,
                            size_t nr_pages);
void vm_remove_region(struct mm_address_space *as, struct vm_area_struct *region);
int vm_add_region(struct mm_address_space *as, struct vm_area_struct *region);
int __vm_munmap(struct mm_address_space *as, void *__addr, size_t size);
bool limits_are_contained(struct vm_area_struct *reg, unsigned long start, unsigned long limit);
bool vm_mapping_is_cow(struct vm_area_struct *entry);

vm_area_struct *vm_search(struct mm_address_space *mm, void *addr, size_t length)
    REQUIRES_SHARED(mm->vm_lock);

/**
 * @brief Finds a vm region.
 *
 * @param as Address space
 * @param addr An address inside the region.
 * @return A pointer to the region, or NULL if it doesn't exist.
 */
__always_inline struct vm_area_struct *vm_find_region(struct mm_address_space *as, void *addr)
    REQUIRES_SHARED(as->vm_lock)
{
    return vm_search(as, addr, 2);
}

bool vm_test_vs_rlimit(const mm_address_space *as, ssize_t diff)
{
    /* The kernel doesn't have resource limits */
    if (as == &kernel_address_space)
        return true;
    /* Decreasing the resource usage doesn't respect limits */
    if (diff < 0)
        return true;
    return get_current_process()->get_rlimit(RLIMIT_AS).rlim_cur >=
           as->virtual_memory_size + (size_t) diff;
}

constinit struct mm_address_space kernel_address_space = {};
static struct page *vm_zero_page = nullptr;
static struct slab_cache *vm_area_struct_cache = nullptr;

static inline vm_area_struct *vma_alloc()
{
    return (vm_area_struct *) kmem_cache_alloc(vm_area_struct_cache, GFP_KERNEL);
}

static inline void vma_free(vm_area_struct *region)
{
    kmem_cache_free(vm_area_struct_cache, (void *) region);
}

bool vm_insert_region(struct mm_address_space *as, struct vm_area_struct *region)
{
    return bst_insert(
        &as->region_tree, &region->vm_tree_node,
        [](struct bst_node *lhs_, struct bst_node *rhs_) -> int {
            auto lhs = container_of(lhs_, vm_area_struct, vm_tree_node);
            auto rhs = container_of(rhs_, vm_area_struct, vm_tree_node);

            if (check_for_overlap(lhs->vm_start, lhs->vm_end - 1, rhs->vm_start, rhs->vm_end - 1))
            {
                panic("vm_insert_region: Region [%lx, %lx] and [%lx, %lx] overlap\n", lhs->vm_start,
                      lhs->vm_end, rhs->vm_start, rhs->vm_end);
                return 0;
            }
            else if (rhs->vm_start > lhs->vm_start)
                return 1;
            else
                return -1;
        });
}

struct vm_area_struct *vm_reserve_region(struct mm_address_space *as, unsigned long start,
                                         size_t size)
{
    MUST_HOLD_MUTEX(&as->vm_lock);

    struct vm_area_struct *region = vma_alloc();
    if (!region)
        return nullptr;

    memset(region, 0, sizeof(*region));

    region->vm_start = start;
    region->vm_end = start + size;
    region->vm_flags = 0;
    bst_node_initialize(&region->vm_tree_node);

    bool success = vm_insert_region(as, region);

    if (!success)
    {
        panic("Could not insert vm region [%lx, %lx]", start, region->vm_end - 1);
    }

    region->vm_mm = as;

    return region;
}

#define DEBUG_VM_1 0
#define DEBUG_VM_2 0
#define DEBUG_VM_3 0

unsigned long vm_allocate_base(struct mm_address_space *as, unsigned long min, size_t size,
                               u64 flags) REQUIRES(as->vm_lock)
{
    MUST_HOLD_MUTEX(&as->vm_lock);

    if (min < as->start)
        min = as->start;

    struct a : bst_node
    {
        unsigned long min;
    } priv;

    priv.min = min;

    auto compare = [](bst_node *node0, bst_node *fake) -> int {
        struct a *priv = (a *) fake;
        auto reg = container_of(node0, vm_area_struct, vm_tree_node);
        auto end = reg->vm_end - 1;
        if (check_for_overlap(reg->vm_start, end, priv->min, priv->min + PAGE_SIZE))
            return 0;
        else if (end >= priv->min)
            return -1;
        else // if (end < priv->min)
            return 1;
    };

    struct bst_node *node = nullptr;
    unsigned long last_end = min;
    struct vm_area_struct *f = nullptr;

    if (min != as->start)
    {
        node = bst_search(&as->region_tree, &priv, compare);
    }
    else
    {
        node = bst_min(&as->region_tree, nullptr);
    }

    if (!node)
        goto done;

    /* Check if there's a gap between the first node
     * and the start of the address space
     */

    f = container_of(node, vm_area_struct, vm_tree_node);

#if DEBUG_VM_1
    printk("Tiniest node: %016lx\n", f->base);
#endif
    if (f->vm_start - min >= size)
    {
#if DEBUG_VM_2
        printk("gap [%016lx - %016lx]\n", min, f->base);
#endif
        goto done;
    }

    while (node)
    {
        f = container_of(node, vm_area_struct, vm_tree_node);
        last_end = f->vm_end;

        node = bst_next(&as->region_tree, node);
        if (!node)
            break;

        struct vm_area_struct *vm = container_of(node, vm_area_struct, vm_tree_node);

        if (vm->vm_start - last_end >= size && min <= vm->vm_start)
            break;
    }

done:
    last_end = last_end < min ? min : last_end;

    // We can't map something over the edge
    if (last_end > as->end || last_end + size > as->end)
        return -1;

    // The architecture may have extra checks to do
    if (!arch_vm_validate_mmap_region(last_end, size, flags))
        return -1;

    return last_end;
}

static struct vm_area_struct *vm_allocate_region(struct mm_address_space *as, unsigned long min,
                                                 size_t size, u64 flags) REQUIRES(as->vm_lock)
{
    if (!vm_test_vs_rlimit(as, size))
        return errno = ENOMEM, nullptr;

    unsigned long new_base = vm_allocate_base(as, min, size, flags);

    if (new_base == (unsigned long) -1)
        return nullptr;

    assert((new_base & (PAGE_SIZE - 1)) == 0);

    struct vm_area_struct *reg = vm_reserve_region(as, new_base, size);

    if (reg)
    {
        increment_vm_stat(as, virtual_memory_size, size);
    }

    return reg;
}

void vm_addr_init()
{
    kernel_address_space.start = VM_HIGHER_HALF;
    kernel_address_space.end = UINTPTR_MAX;

    // Permanent reference
    kernel_address_space.ref();
}

static inline bool is_higher_half(void *address)
{
    return (uintptr_t) address > VM_HIGHER_HALF;
}

/**
 * @brief Initialises the early architecture dependent parts of the VM subsystem.
 *
 */
void vm_init()
{
    paging_init();
    arch_vm_init();

    // Reserve the kernel in the boot memory map
    struct kernel_limits limits;
    get_kernel_limits(&limits);

    bootmem_reserve(limits.start_phys, limits.end_phys - limits.start_phys);
}

/**
 * @brief Initialises the architecture independent parts of the VM subsystem.
 *
 */
void vm_late_init()
{
    /* TODO: This should be arch specific stuff, move this to arch/ */
    const auto vmalloc_noaslr = vmalloc_space;
    vmalloc_space = vm_randomize_address(vmalloc_space, VMALLOC_ASLR_BITS);

    // Initialize vmalloc first. This will feed the rest of the allocators.
    const auto vmalloc_len = VM_VMALLOC_SIZE - (vmalloc_space - vmalloc_noaslr);

    vmalloc_init(vmalloc_space, vmalloc_len);
    // Now initialize slabs for kmalloc

    kmalloc_init();

    vm_area_struct_cache =
        kmem_cache_create("vm_area_struct", sizeof(vm_area_struct), 0, 0, nullptr);

    if (!vm_area_struct_cache)
        panic("vm: early boot oom");

    vm_addr_init();

    scoped_mutex g{kernel_address_space.vm_lock};

    /* Start populating the address space */
    struct kernel_limits l;
    get_kernel_limits(&l);
    size_t kernel_size = l.end_virt - l.start_virt;

    struct vm_area_struct *v = vm_reserve_region(&kernel_address_space, l.start_virt, kernel_size);
    if (!v)
    {
        panic("vmm: early boot oom");
    }

    v->vm_flags = VM_WRITE | VM_READ | VM_EXEC;

    v = vm_reserve_region(&kernel_address_space, vmalloc_space, vmalloc_len);

    if (!v)
    {
        panic("vmm: early boot oom");
    }

    v->vm_flags = VM_WRITE | VM_READ;

    vm_zero_page = alloc_page(0);
    assert(vm_zero_page != nullptr);

    is_initialized = true;
}

/**
 * @brief Maps a range of memory with freshly allocated anonymous pages.
 * This should only be used by very-specific MM or MM related code.
 * @param range Virtual address.
 * @param pages Number of pages to be mapped.
 * @param flags Protection on the mappings.
 * @return The list of allocated pages, or NULL if there was an out of memory scenario.
 */
struct page *vm_map_range(void *range, size_t nr_pages, uint64_t flags)
{
    const unsigned long mem = (unsigned long) range;
    struct page *pages = alloc_page_list(nr_pages, 0);
    struct page *p = pages;
    if (!pages)
        goto out_of_mem;

#ifdef DEBUG_PRINT_MAPPING
    printk("vm_map_range: %p - %lx\n", range, (unsigned long) range + nr_pages << PAGE_SHIFT);
#endif

    for (size_t i = 0; i < nr_pages; i++)
    {
        // printf("Mapping %p\n", p->paddr);
        if (!vm_map_page(nullptr, mem + (i << PAGE_SHIFT), (uintptr_t) page_to_phys(p), flags))
            goto out_of_mem;
        p = p->next_un.next_allocation;
    }

    return pages;

out_of_mem:
    if (pages)
        free_page_list(pages);
    return nullptr;
}

void do_vm_unmap(struct mm_address_space *as, void *range, size_t pages)
    REQUIRES_SHARED(as->vm_lock)
{
    struct vm_area_struct *entry = vm_find_region(as, range);
    assert(entry != nullptr);

    MUST_HOLD_MUTEX(&entry->vm_mm->vm_lock);

    vm_mmu_unmap(entry->vm_mm, range, pages);
}

void __vm_unmap_range(struct mm_address_space *as, void *range, size_t pages)
    REQUIRES_SHARED(as->vm_lock)
{
    do_vm_unmap(as, range, pages);
}

static inline bool inode_requires_wb(struct inode *i)
{
    return true;
}

bool vm_mapping_requires_wb(struct vm_area_struct *reg)
{
    return reg->vm_maptype == MAP_SHARED && reg->vm_file && inode_requires_wb(reg->vm_file->f_ino);
}

bool vm_mapping_is_anon(struct vm_area_struct *reg)
{
    return reg->vm_file == nullptr;
}

/**
 * @brief Transforms a file-backed region into an anonymously backed one.
 *
 * @param region A pointer to the vm_area_struct.
 */
void vm_make_anon(struct vm_area_struct *reg)
{
    if (reg->vm_file)
    {
        fd_put(reg->vm_file);
        reg->vm_file = nullptr;
    }

    reg->vm_maptype |= MAP_ANONYMOUS;
}

bool vm_mapping_requires_write_protect(struct vm_area_struct *reg)
{
    return vm_mapping_requires_wb(reg);
}

void vm_area_struct_destroy(struct vm_area_struct *region)
{
    MUST_HOLD_MUTEX(&region->vm_mm->vm_lock);

    /* First, unref things */
    if (region->vm_file)
    {
        fd_put(region->vm_file);
    }

    if (region->vm_obj)
    {
        vmo_remove_mapping(region->vm_obj, region);
        vmo_unref(region->vm_obj);
    }

    if (region->vm_amap)
    {
        amap_unref(region->vm_amap);
    }

    memset_explicit(region, 0xfd, sizeof(struct vm_area_struct));

    vma_free(region);
}

unsigned long vm_get_base_address(uint64_t flags, uint32_t type)
{
    bool is_kernel_map = flags & VM_KERNEL;
    struct mm_address_space *mm = nullptr;

    if (!is_kernel_map)
    {
        mm = get_current_address_space();
    }

    switch (type)
    {
        case VM_TYPE_SHARED:
        case VM_TYPE_STACK: {
            DCHECK(!is_kernel_map);
            return (uintptr_t) mm->mmap_base;
        }

        case VM_TYPE_MODULE: {
            assert(is_kernel_map == true);

            return KERNEL_VIRTUAL_BASE;
        }

        default:
        case VM_TYPE_REGULAR: {
            if (is_kernel_map)
                return vmalloc_space;
            else
                return (uintptr_t) mm->mmap_base;
        }
    }
}

static struct vm_area_struct *__vm_allocate_virt_region(struct mm_address_space *as, uint64_t flags,
                                                        size_t pages, uint32_t type, uint64_t prot)
    REQUIRES(as->vm_lock)
{
    MUST_HOLD_MUTEX(&as->vm_lock);

    unsigned long base_addr = vm_get_base_address(flags, type);
    struct vm_area_struct *region = vm_allocate_region(as, base_addr, pages << PAGE_SHIFT, flags);

    if (region)
    {
        if (prot & (VM_WRITE | VM_EXEC))
            prot |= VM_READ;
        region->vm_flags = prot;
    }

    return region;
}

vm_area_struct *vm_search(struct mm_address_space *mm, void *addr, size_t length)
    REQUIRES_SHARED(mm->vm_lock)
{
    struct search_type
    {
        unsigned long base;
        size_t pages;
        struct bst_node node;
    } search;

    search.base = (unsigned long) addr & -PAGE_SIZE;
    search.pages = vm_size_to_pages(length);

    auto node = bst_search(&mm->region_tree, &search.node,
                           [](struct bst_node *lhs_, struct bst_node *rhs_) -> int {
                               auto lhs = container_of(lhs_, vm_area_struct, vm_tree_node);
                               auto rhs = container_of(rhs_, search_type, node);

                               if (check_for_overlap(lhs->vm_start, lhs->vm_end - 1, rhs->base,
                                                     rhs->base + (rhs->pages << PAGE_SHIFT) - 1))
                               {
                                   return 0;
                               }
                               else if (rhs->base > lhs->vm_start)
                                   return 1;
                               else
                                   return -1;
                           });

    return containerof_null_safe(node, vm_area_struct, vm_tree_node);
}

#define VM_CREATE_REGION_AT_DEBUG 0

struct vm_area_struct *__vm_create_region_at(struct mm_address_space *mm, void *addr, size_t pages,
                                             uint32_t type, uint64_t prot) REQUIRES(mm->vm_lock)
{
    struct vm_area_struct *v = nullptr;

    if (vm_search(mm, addr, pages << PAGE_SHIFT) != nullptr)
    {
        /* We found a region, this is not empty! */

#if VM_CREATE_REGION_AT_DEBUG
        sched_enable_preempt();
        printk("Failed to map %p - %lx\n", addr, (unsigned long) addr + (pages << PAGE_SHIFT));
        vm_print_umap();
        sched_disable_preempt();
#endif

        errno = EINVAL;
        return nullptr;
    }

    if (!vm_test_vs_rlimit(mm, pages << PAGE_SHIFT))
    {
        return errno = ENOMEM, nullptr;
    }

    v = vm_reserve_region(mm, (unsigned long) addr, pages << PAGE_SHIFT);
    if (!v)
    {
        addr = nullptr;
        errno = ENOMEM;
        goto return_;
    }

    increment_vm_stat(mm, virtual_memory_size, pages << PAGE_SHIFT);

    v->vm_start = (unsigned long) addr;
    v->vm_end = v->vm_start + (pages << PAGE_SHIFT);
    v->vm_flags = prot;

return_:
    return v;
}

/**
 * @brief Creats a new address space.
 *
 * @param addr_space A pointer to the new address space.
 * @return 0 on success, negative on error.
 */
int vm_clone_as(mm_address_space *addr_space, mm_address_space *original)
{
    if (!original)
        original = get_current_address_space();
    if (!original)
        original = &kernel_address_space;
    scoped_mutex g{original->vm_lock};
    return paging_clone_as(addr_space, original);
}

struct fork_iteration
{
    struct mm_address_space *target_mm;
    bool success;
};

#define DEBUG_FORK_VM 0
static bool fork_vm_area_struct(struct vm_area_struct *region, struct fork_iteration *it)
{
    bool vmo_failure, is_private, needs_to_fork_memory;
    bool res;

    struct vm_area_struct *new_region = vma_alloc();
    if (!new_region)
    {
        goto ohno;
    }

    memcpy(new_region, region, sizeof(*region));

#if DEBUG_FORK_VM
    printk("Forking [%016lx, %016lx] perms %x\n", region->base,
           region->base + (region->pages << PAGE_SHIFT) - 1, region->rwx);
#endif
    bst_node_initialize(&new_region->vm_tree_node);

    res = vm_insert_region(it->target_mm, new_region);

    assert(res == true);

    if (new_region->vm_file)
        fd_get(new_region->vm_file);

    vmo_failure = false;
    is_private = !is_mapping_shared(new_region);
    needs_to_fork_memory = is_private;

    if (needs_to_fork_memory)
    {
        /* No need to ref the vmo since it was a new vmo created for us while forking. */
        if (new_region->vm_obj)
        {
            vmo_assign_mapping(new_region->vm_obj, new_region);
            vmo_ref(new_region->vm_obj);
        }

        if (new_region->vm_amap)
        {
            amap_ref(new_region->vm_amap);
        }
    }
    else
    {
        vmo_ref(new_region->vm_obj);
        vmo_assign_mapping(new_region->vm_obj, new_region);
    }

    if (vmo_failure)
    {
        vm_remove_region(it->target_mm, new_region);
        vma_free(new_region);
        goto ohno;
    }

    new_region->vm_mm = it->target_mm;

    if (mmu_fork_tables(region, it->target_mm) < 0)
        goto ohno;
    mmu_verify_address_space_accounting(it->target_mm);
    return true;

ohno:
    it->success = false;
    return false;
}

static void addr_space_delete(vm_area_struct *region) NO_THREAD_SAFETY_ANALYSIS
{
    // NO_THREAD_SAFETY_ANALYSIS = we can do this without holding the lock, as tear_down_addr_space
    // is called in fork paths.
    do_vm_unmap(region->vm_mm, (void *) region->vm_start, vma_pages(region));

    vm_area_struct_destroy(region);
}

static void tear_down_addr_space(struct mm_address_space *addr_space)
{
    /*
     * Note: We free the tree first in order to free any forked pages.
     * If we didn't we would leak some memory.
     */
    vm_area_struct *entry;
    bst_for_every_entry_delete(&addr_space->region_tree, entry, vm_area_struct, vm_tree_node)
    {
        addr_space_delete(entry);
    }

    paging_free_page_tables(addr_space);
}

/**
 * @brief Fork the current address space into a new address space.
 *
 * @param addr_space The new address space.
 * @return 0 on success, negative on error.
 */
int vm_fork_address_space(struct mm_address_space *addr_space) EXCLUDES(addr_space->vm_lock)
    EXCLUDES(get_current_address_space()->vm_lock)
{
    struct mm_address_space *current_mm = get_current_address_space();
    scoped_mutex g{current_mm->vm_lock};

#if CONFIG_DEBUG_ADDRESS_SPACE_ACCT
    mmu_verify_address_space_accounting(get_current_address_space());
#endif

    struct fork_iteration it = {};
    it.target_mm = addr_space;
    it.success = true;

    if (paging_clone_as(addr_space, current_mm) < 0)
        return -ENOMEM;

    bst_root_initialize(&addr_space->region_tree);

    addr_space->resident_set_size = 0;
    addr_space->virtual_memory_size = current_mm->virtual_memory_size;

    vm_area_struct *entry;
    bst_for_every_entry(&current_mm->region_tree, entry, vm_area_struct, vm_tree_node)
    {
        if (!fork_vm_area_struct(entry, &it))
        {
            tear_down_addr_space(addr_space);
            return -1;
        }
    }

    addr_space->shared_set_size = current_mm->shared_set_size;

    /* We add the old ones here because rss will only be incremented by pages that were newly
     * mapped, since the old page table entries were copied in.
     */
    addr_space->mmap_base = current_mm->mmap_base;
    addr_space->brk = current_mm->brk;
    addr_space->start = current_mm->start;
    addr_space->end = current_mm->end;

#if CONFIG_DEBUG_ADDRESS_SPACE_ACCT
    mmu_verify_address_space_accounting(addr_space);
#endif

    assert(addr_space->active_mask.is_empty());

    mutex_init(&addr_space->vm_lock);

    return 0;
}

/**
 * @brief Changes permissions of a memory area.
 * Note: Deprecated and should not be used.
 * @param range Start of the range.
 * @param pages Number of pages.
 * @param perms New permissions.
 */
void vm_change_perms(void *range, size_t pages, int perms) NO_THREAD_SAFETY_ANALYSIS
{
    struct mm_address_space *as;
    bool kernel = is_higher_half(range);
    bool needs_release = false;
    if (kernel)
        as = &kernel_address_space;
    else
        as = get_current_process()->get_aspace();

    if (mutex_owner(&as->vm_lock) != get_current_thread())
    {
        needs_release = true;
        mutex_lock(&as->vm_lock);
    }

    for (size_t i = 0; i < pages; i++)
    {
        paging_change_perms(range, perms);

        range = (void *) ((unsigned long) range + PAGE_SIZE);
    }

    vm_invalidate_range((unsigned long) range, pages);

    if (needs_release)
        mutex_unlock(&as->vm_lock);
}

bool vm_may_merge_with_adj(vm_area_struct *reg)
{
    // TODO: merging is broken right now
    return false;
#if 0
    auto prev_node = bst_prev(&reg->mm->region_tree, &reg->tree_node);

    if (!prev_node)
        return false;

    auto prev = container_of(prev_node, vm_area_struct, tree_node);

    if (vmo_is_shared(prev->vmo))
        return false;

    return (false && prev->mapping_type == reg->mapping_type && reg->type == prev->type &&
            prev->base + (prev->pages << PAGE_SHIFT) == reg->base && prev->fd == reg->fd &&
            reg->fd == nullptr && reg->rwx == prev->rwx && !is_mapping_shared(reg) &&
            !is_file_backed(reg));
#endif
}

void vm_merge_with_prev(vm_area_struct *reg)
{
#if 0
    auto prev_node = bst_prev(&reg->vm_mm->region_tree, &reg->vm_tree_node);

    assert(prev_node);
    auto prev = container_of(prev_node, vm_area_struct, vm_tree_node);

#if 0
    printk("[%lx, %lx] + [%lx, %lx] =", prev->base, prev->base + (prev->pages << PAGE_SHIFT),
           reg->base, reg->base + (reg->pages << PAGE_SHIFT));
#endif
    prev->vm_pages += reg->vm_pages;
    auto oldsize = prev->vm_obj->size;
    prev->vm_obj->size = oldsize + (reg->vm_pages << PAGE_SHIFT);
    // vmo_truncate(prev->vmo, oldsize + ((reg->pages + 200) << PAGE_SHIFT), 0);
    vm_remove_region(reg->vm_mm, reg);
#if 0
    printk(" [%lx, %lx]\n", prev->base, prev->base + (prev->pages << PAGE_SHIFT));
    printk("vmo size %lx -> %lx\n", oldsize, prev->vmo->size);
#endif
    vma_free(reg);
#endif
}

/**
 * @brief Creates a new user-space mapping.
 * Note: This is mmap(2)'s backend and therefore, most semantics are shared
 * between the two.
 *
 * @param addr An optional address hint.
 * @param length The length of the mapping, in bytes.
 * @param prot The desired protection flags (see PROT_* as in mmap(2)).
 * @param flags The mapping flags (see MAP_* as in mmap(2)).
 * @param file An optional pointer to a file, if it is a file mapping.
 * @param off The offset into the file, if it is a file mapping.
 * @return A pointer to the new memory mapping, or NULL if it failed (errno is set).
 */
void *vm_mmap(void *addr, size_t length, int prot, int flags, struct file *file, off_t off)
{
    int st = 0;
    struct vm_area_struct *area = nullptr;
    bool is_file_mapping = file != nullptr;
    void *base = nullptr;
    u64 extra_flags = 0;

    struct mm_address_space *mm = get_current_address_space();

    /* We don't like this offset. */
    if (off & (PAGE_SIZE - 1))
        return errno = EINVAL, nullptr;

    scoped_mutex g{mm->vm_lock};

    /* Calculate the pages needed for the overall size */
    size_t pages = vm_size_to_pages(length);

    if (prot & (PROT_WRITE | PROT_EXEC))
        prot |= PROT_READ;

    int vm_prot = VM_USER | ((prot & PROT_READ) ? VM_READ : 0) |
                  ((prot & PROT_WRITE) ? VM_WRITE : 0) | ((prot & PROT_EXEC) ? VM_EXEC : 0);

    /* Sanitize the address and length */
    const auto aligned_len = pages << PAGE_SHIFT;

    if (aligned_len > arch_low_half_max)
    {
        st = -ENOMEM;
        goto out_error;
    }

    if (is_higher_half(addr) || (unsigned long) addr & (PAGE_SIZE - 1) ||
        (unsigned long) addr > arch_low_half_max - aligned_len)
    {
        if (flags & MAP_FIXED)
        {
            st = -ENOMEM;
            goto out_error;
        }
        else
            addr = nullptr;
    }

    extra_flags = arch_vm_interpret_mmap_hint_flags(addr, flags);

    if (!addr)
    {
        if (flags & MAP_FIXED)
        {
            st = -ENOMEM;
            goto out_error;
        }
        /* Specified by POSIX, if addr == nullptr, guess an address */
        area = __vm_allocate_virt_region(mm, VM_ADDRESS_USER | extra_flags, pages, VM_TYPE_SHARED,
                                         vm_prot);
    }
    else
    {
        if (flags & MAP_FIXED)
        {
            __vm_munmap(mm, addr, pages << PAGE_SHIFT);
        }

        area = __vm_create_region_at(mm, addr, pages, VM_TYPE_REGULAR, vm_prot);
        if (!area)
        {
            if (flags & MAP_FIXED)
            {
                st = -ENOMEM;
                goto out_error;
            }

            area = __vm_allocate_virt_region(mm, VM_ADDRESS_USER | extra_flags, pages,
                                             VM_TYPE_REGULAR, vm_prot);
        }
    }

    if (!area)
    {
        st = -ENOMEM;
        goto out_error;
    }

    if (flags & MAP_SHARED)
        area->vm_maptype = MAP_SHARED;
    else
        area->vm_maptype = MAP_PRIVATE;

    if (is_file_mapping)
    {
        // printk("Mapping off %lx, size %lx, prots %x\n", off, length, prot);

        /* Set additional meta-data */

        area->vm_offset = off;
        area->vm_file = file;

        fd_get(file);

        struct inode *ino = file->f_ino;

        if (S_ISBLK(ino->i_mode) || S_ISCHR(ino->i_mode))
        {
            if (!ino->i_fops->mmap)
            {
                __vm_munmap(mm, (void *) area->vm_start, pages << PAGE_SHIFT);
                return errno = ENODEV, nullptr;
            }

            void *ret = ino->i_fops->mmap(area, file);

            if (ret)
                inode_update_atime(ino);
            else
                __vm_munmap(mm, (void *) area->vm_start, pages << PAGE_SHIFT);

            if (ret)
                goto out;

            return ret;
        }
    }
    else if (vm_may_merge_with_adj(area))
    {
        base = (void *) area->vm_start;
        vm_merge_with_prev(area);
        return base;
    }

    if (vm_area_struct_setup_backing(area, pages, !(flags & MAP_ANONYMOUS)) < 0)
    {
        __vm_munmap(mm, (void *) area->vm_start, pages << PAGE_SHIFT);
        return errno = ENOMEM, nullptr;
    }

out:
    if (flags & MAP_SHARED)
        increment_vm_stat(mm, shared_set_size, pages << PAGE_SHIFT);

    base = (void *) area->vm_start;

    return base;

out_error:
    return errno = -st, nullptr;
}

void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t off)
{
    int error = 0;

    struct file *file = nullptr;
    void *ret = nullptr;
    bool is_file_mapping = !(flags & MAP_ANONYMOUS);

    /* Ok, start the basic input sanitation for user-space inputs */
    if (length == 0)
        return (void *) -EINVAL;

    if (!(flags & MAP_PRIVATE) && !(flags & MAP_SHARED))
        return (void *) -EINVAL;

    if (flags & MAP_PRIVATE && flags & MAP_SHARED)
        return (void *) -EINVAL;

    /* Our mmap() implementation needs page aligned offsets */
    if (off % PAGE_SIZE)
        return (void *) -EINVAL;

    if (is_file_mapping) /* This is a file-backed mapping */
    {
        file = get_file_description(fd);
        if (!file)
            return (void *) (unsigned long) -errno;

        /* You can't map a file without having read access to it. */
        if (!fd_may_access(file, FILE_ACCESS_READ))
        {
            error = -EACCES;
            goto out_error;
        }

        bool fd_has_write = fd_may_access(file, FILE_ACCESS_WRITE);

        /* You can't create a shared mapping of a file without having write access to it */
        if (!fd_has_write && prot & PROT_WRITE && flags & MAP_SHARED)
        {
            error = -EACCES;
            goto out_error;
        }
    }

    ret = vm_mmap(addr, length, prot, flags, file, off);

    if (ret == nullptr)
    {
        ret = (void *) (unsigned long) -errno;
    }

    if (file)
        fd_put(file);

    return ret;
out_error:
    if (file)
        fd_put(file);
    return (void *) (unsigned long) error;
}

int sys_munmap(void *addr, size_t length)
{
    // printk("munmap [%p, %lx]\n", addr, (unsigned long) addr + length - 1);

    if (is_higher_half(addr))
        return -EINVAL;

    size_t pages = vm_size_to_pages(length);

    if ((unsigned long) addr & (PAGE_SIZE - 1))
        return -EINVAL;

    struct mm_address_space *mm = get_current_process()->get_aspace();

    int ret = vm_munmap(mm, addr, pages << PAGE_SHIFT);

    return ret;
}

void vm_copy_region(const struct vm_area_struct *source, struct vm_area_struct *dest,
                    int copy_amap = 1)
{
    dest->vm_file = source->vm_file;
    if (dest->vm_file)
    {
        /*struct file *ino = dest->fd->f_ino;
        if(source->mapping_type == MAP_SHARED && inode_requires_wb(ino))
            writeback_add_region(dest);*/
        fd_get(dest->vm_file);
    }

    dest->vm_flags = source->vm_flags;
    dest->vm_maptype = source->vm_maptype;
    dest->vm_offset = source->vm_offset;
    dest->vm_mm = source->vm_mm;
    dest->vm_obj = source->vm_obj;
    if (dest->vm_obj)
        vmo_ref(dest->vm_obj);

    if (copy_amap)
    {
        dest->vm_amap = source->vm_amap;
        if (dest->vm_amap)
            amap_ref(dest->vm_amap);
    }

    dest->vm_ops = source->vm_ops;
}

static struct vm_area_struct *vm_split_region(struct mm_address_space *as,
                                              struct vm_area_struct *vma, unsigned long addr,
                                              bool below) REQUIRES(as->vm_lock)
{
    DCHECK((addr & (PAGE_SIZE - 1)) == 0);
    size_t region_off = addr - vma->vm_start;
    size_t off_pages = region_off >> PAGE_SHIFT;
    struct vm_area_struct *newr = vma_alloc();
    if (!newr)
        return nullptr;

    memset(newr, 0, sizeof(*newr));
    vm_copy_region(vma, newr, 0);

    if (vma->vm_amap)
    {
        auto new_amap = amap_split(vma->vm_amap, vma, off_pages);
        if (!new_amap)
        {
            vma_free(newr);
            return nullptr;
        }

        /* Below region gets the original 'vma->vm_amap', top gets the new */
        if (below)
        {
            newr->vm_amap = vma->vm_amap;
            vma->vm_amap = new_amap;
        }
        else
            newr->vm_amap = new_amap;
    }

    DCHECK(vma->vm_end > addr);

    if (below)
    {
        newr->vm_start = vma->vm_start;
        newr->vm_end = addr;
        vma->vm_start = addr;
        vma->vm_offset += region_off;
        if (vma->vm_obj)
        {
            vm_obj_reassign_mapping(vma->vm_obj, vma);
            vmo_assign_mapping(vma->vm_obj, newr);
        }
    }
    else
    {
        newr->vm_start = addr;
        newr->vm_end = vma->vm_end;
        newr->vm_offset += region_off;
        vma->vm_end = addr;
        if (vma->vm_obj)
            vmo_assign_mapping(vma->vm_obj, newr);
    }

    vm_insert_region(as, newr);

    return newr;
}

static void vm_mprotect_handle_prot(struct vm_area_struct *region, int *pprot)
{
    int prot = *pprot;
    bool marking_write = (prot & VM_WRITE) && !(region->vm_flags & VM_WRITE);

    region->vm_flags = prot;

    if (marking_write && (vm_mapping_is_cow(region) || vm_mapping_requires_write_protect(region)))
    {
        /* If we're a COW mapping or some kind of mapping that requires write-protection,
         * we can't change the pages' permissions to allow VM_WRITE
         */
        *pprot &= ~VM_WRITE;
    }
}

void vm_do_mmu_mprotect(struct mm_address_space *as, void *address, size_t nr_pgs, int old_prots,
                        int new_prots)
{
    void *addr = address;

    for (size_t i = 0; i < nr_pgs; i++)
    {
        vm_mmu_mprotect_page(as, address, old_prots, new_prots);

        address = (void *) ((unsigned long) address + PAGE_SIZE);
    }

    vm_invalidate_range((unsigned long) addr, nr_pgs);
}

/**
 * @brief Changes memory protection of a memory range.
 *
 * @param as The target address space.
 * @param __addr The pointer to the start of the memory range.
 * @param size The size of the memory range, in bytes.
 * @param prot The desired protection flags. Valid flags are PROT_*, as in mmap(2)
               or mprotect(2).
 * @return 0 on success, negative error codes.
 */
int vm_mprotect(struct mm_address_space *as, void *__addr, size_t size, int prot)
    EXCLUDES(as->vm_lock)
{
    unsigned long addr = (unsigned long) __addr;
    unsigned long limit = addr + size;

    scoped_mutex g{as->vm_lock};

    /* Note: vm_munmap has some vma detaching logic for the simple fact that POSIX does not
     * allow for a partial unmap in case of an error. Whereas this is not the case for mprotect.
     */

    struct vm_area_struct *region = vm_search(as, (void *) addr, PAGE_SIZE);
    if (!region)
        return -ENOMEM;

    /* Split the head, if needed */
    if (addr > region->vm_start)
    {
        region = vm_split_region(as, region, addr, false);
        if (!region)
            return -ENOMEM;
        DCHECK(addr == region->vm_start);
    }

    while (region)
    {
        /* Split the tail, if needed */
        if (limit < region->vm_end)
        {
            region = vm_split_region(as, region, limit, true);
            if (!region)
                return -ENOMEM;
            DCHECK(limit == region->vm_end);
        }

        if (region->vm_maptype == MAP_SHARED && region->vm_file && prot & PROT_WRITE)
        {
            /* Block the mapping if we're trying to mprotect a shared mapping to PROT_WRITE while
             * not having the necessary perms on the file.
             */

            struct file *file = region->vm_file;
            bool fd_has_write = fd_may_access(file, FILE_ACCESS_WRITE);

            if (!fd_has_write)
                return -EACCES;
        }

        int old_prots = region->vm_flags;
        int new_prots = prot;
        unsigned long vma_end = region->vm_end;
        unsigned long end = ALIGN_TO(min(addr + size, vma_end), 4096);
        unsigned long mprotect_size = end - addr;
        DCHECK((mprotect_size & (PAGE_SIZE - 1)) == 0);

        vm_mprotect_handle_prot(region, &new_prots);
        vm_do_mmu_mprotect(as, (void *) addr, mprotect_size >> PAGE_SHIFT, old_prots, new_prots);

        if (region->vm_end == limit)
            break;

        addr += mprotect_size;
        size -= mprotect_size;
        region = containerof_null_safe(bst_next(&as->region_tree, &region->vm_tree_node),
                                       struct vm_area_struct, vm_tree_node);
    }

    return 0;
}

int sys_mprotect(void *addr, size_t len, int prot)
{
    if (is_higher_half(addr))
        return -EINVAL;

    /* The address needs to be page aligned */
    if ((unsigned long) addr & (PAGE_SIZE - 1))
        return -EINVAL;

    /* Error on len misalignment */
    if (len & (PAGE_SIZE - 1))
        return -EINVAL;

    int vm_prot = VM_USER | ((prot & PROT_WRITE) ? VM_WRITE : 0) |
                  ((prot & PROT_EXEC) ? VM_EXEC : 0) | ((prot & PROT_READ) ? VM_READ : 0);

    if (prot & PROT_WRITE)
        vm_prot |= VM_READ;

    size_t pages = vm_size_to_pages(len);

    len = pages << PAGE_SHIFT; /* Align len on a page boundary */

    struct process *p = get_current_process();

    return vm_mprotect(p->address_space.get(), addr, len, vm_prot);
}

int vm_expand_brk(struct mm_address_space *as, size_t nr_pages) REQUIRES(as->vm_lock);

__always_inline int do_inc_brk(mm_address_space *as, void *oldbrk, void *newbrk)
    REQUIRES(as->vm_lock)
{
    void *oldpage = page_align_up(oldbrk);
    void *newpage = page_align_up(newbrk);

    size_t pages = ((uintptr_t) newpage - (uintptr_t) oldpage) / PAGE_SIZE;

    if (pages > 0)
    {
        return vm_expand_brk(as, pages);
    }

    return 0;
}

uint64_t sys_brk(void *newbrk)
{
    mm_address_space *as = get_current_address_space();

    scoped_mutex g{as->vm_lock};

    if (newbrk == nullptr)
    {
        uint64_t ret = (uint64_t) as->brk;
        return ret;
    }

    void *old_brk = as->brk;
    ptrdiff_t diff = (ptrdiff_t) newbrk - (ptrdiff_t) old_brk;

    if (diff < 0)
    {
        /* TODO: Implement freeing memory with brk(2) */
        as->brk = newbrk;
    }
    else
    {
        /* Increment the program brk */
        if (do_inc_brk(as, old_brk, newbrk) < 0)
        {
            return -ENOMEM;
        }

        as->brk = newbrk;
    }

    uint64_t ret = (uint64_t) as->brk;
    return ret;
}

static bool vm_print(struct vm_area_struct *region)
{
    bool x = region->vm_flags & VM_EXEC;
    bool w = region->vm_flags & VM_WRITE;
    bool file_backed = is_file_backed(region);
    struct file *fd = region->vm_file;

    printk("[%016lx - %016lx] : %s%s%s ", region->vm_start, region->vm_end, "R", w ? "W" : "-",
           x ? "X" : "-");
    printk("vmo %p mapped at offset %lx", region->vm_obj, region->vm_offset);
    if (file_backed)
        printk(" - file backed ino %lu\n", fd->f_ino->i_inode);
    else
        printk("\n");

    return true;
}

/**
 * @brief Traverses the kernel's memory map and prints information.
 *
 */
void vm_print_map(void)
{
    // rb_tree_traverse(kernel_address_space.area_tree, vm_print, nullptr);
}

/**
 * @brief Traverses the current process's memory map and prints information.
 *
 */
void vm_print_umap()
{
    vm_for_every_region(*get_current_address_space(), vm_print);
    printk("brk: %p\n", get_current_address_space()->brk);
}

#define DEBUG_PRINT_MAPPING 0

/**
 * @brief Map a specific number of pages onto a virtual address.
 * Should only be used by MM code since it does not touch vm_area_structs, only
 * MMU page tables.
 *
 * @param as   The target address space.
 * @param virt The virtual address.
 * @param phys The start of the physical range.
 * @param size The size of the mapping, in bytes.
 * @param flags The permissions on the mapping.
 *
 * @return NULL on error, virt on success.
 */
void *__map_pages_to_vaddr(struct mm_address_space *as, void *virt, void *phys, size_t size,
                           size_t flags)
{
    if (flags & VM_WRITE)
        assert((unsigned long) phys != (unsigned long) page_to_phys(vm_zero_page));

    size_t pages = vm_size_to_pages(size);

#if DEBUG_PRINT_MAPPING
    printk("__map_pages_to_vaddr: %p (phys %p) - %lx\n", virt, phys, (unsigned long) virt + size);
#endif
    void *ptr = virt;
    for (uintptr_t virt = (uintptr_t) ptr, _phys = (uintptr_t) phys, i = 0; i < pages;
         virt += PAGE_SIZE, _phys += PAGE_SIZE, ++i)
    {
        if (!vm_map_page(as, virt, _phys, flags))
            return nullptr;
    }

    if (!(flags & VM_NOFLUSH))
        vm_invalidate_range((unsigned long) virt, pages);

    return ptr;
}

/**
 * @brief Map a specific number of pages onto a virtual address.
 * Should only be used by MM code since it does not touch vm_area_structs, only
 * MMU page tables.
 *
 * @param virt The virtual address.
 * @param phys The start of the physical range.
 * @param size The size of the mapping, in bytes.
 * @param flags The permissions on the mapping.
 *
 * @return NULL on error, virt on success.
 */
void *map_pages_to_vaddr(void *virt, void *phys, size_t size, size_t flags)
{
    return __map_pages_to_vaddr(nullptr, virt, phys, size, flags);
}

static int vm_pf_get_page_from_vmo(struct vm_pf_context *ctx)
{
    struct vm_area_struct *entry = ctx->entry;
    struct inode *ino = entry->vm_file->f_ino;
    size_t vmo_off = (ctx->vpage - entry->vm_start) + entry->vm_offset;
    DCHECK(entry->vm_file != nullptr);

    /* SIGBUS! */
    if (vmo_off >= ino->i_size)
        return -ENOENT;

    return filemap_find_page(ino, vmo_off >> PAGE_SHIFT, 0, &ctx->page,
                             &entry->vm_file->f_ra_state);
}

static int find_page_err_to_signal(int st)
{
    if (st == 0) [[unlikely]]
        return 0;

    switch (st)
    {
        case -ENOENT:
        case -EIO:
            return VM_SIGBUS;
        case -ENOMEM:
        default:
            return VM_SIGSEGV;
    }
}

static int vm_prepare_write(struct inode *inode, struct page *p)
{
    /* TODO: All of this needs a good rework. We must be careful with i_size (we can't just allocate
     * on a whole page like this). We need to retry if the page was truncated. This should not be
     * core vm.cpp code. */
    lock_page(p);

    /* Correctness: We set the i_size before truncating pages from the page cache, so this should
     * not race... I think? */
    size_t i_size = inode->i_size;
    if (p->owner != inode->i_pages)
    {
        pr_warn("vm: (inode %lu, dev %lu) just had a truncate race, which is not yet handled "
                "correctly...\n",
                inode->i_inode, inode->i_dev);
        unlock_page(p);
        return -ENOENT;
    }

    size_t len = PAGE_SIZE;
    size_t offset = p->pageoff << PAGE_SHIFT;
    if (offset + PAGE_SIZE > i_size)
        len = i_size - offset;

    int st = inode->i_fops->prepare_write(inode, p, offset, 0, len);
    filemap_mark_dirty(inode, p, p->pageoff);
    unlock_page(p);
    return st;
}

int vm_handle_non_present_wp(struct fault_info *info, struct vm_pf_context *ctx)
{
    struct vm_area_struct *entry = ctx->entry;

    assert(info->read ^ info->write);
    if (!info->write)
    {
        /* If we'll need to wp, write-protect */
        ctx->page_rwx &= ~VM_WRITE;
        if (vm_mapping_is_anon(entry))
        {
            ctx->page = vm_zero_page;
        }
    }
    else
    {
        if (vm_mapping_requires_wb(entry))
        {
            /* else handle it differently(we'll need) */
            int st = vm_pf_get_page_from_vmo(ctx);
            if (st < 0)
            {
                info->signal = find_page_err_to_signal(st);
                return -1;
            }

            st = vm_prepare_write(ctx->entry->vm_file->f_ino, ctx->page);
            if (st < 0)
            {
                page_unref(ctx->page);
                ctx->page = nullptr;
                info->signal = find_page_err_to_signal(st);
                return -1;
            }
        }
        else if (vm_mapping_is_anon(entry))
        {
            /* This is done in vm_pf_get_page_from_vmo */
        }
    }

    return 0;
}

bool vm_mapping_is_cow(struct vm_area_struct *entry)
{
    return entry->vm_maptype == MAP_PRIVATE;
}

int vm_handle_non_present_pf(struct vm_pf_context *ctx)
{
    struct vm_area_struct *entry = ctx->entry;
    struct fault_info *info = ctx->info;

    if (vm_mapping_requires_write_protect(entry))
    {
        if (vm_handle_non_present_wp(info, ctx) < 0)
            return -1;
    }

    /* If page wasn't set before by other fault handling code, just fetch from the vmo */
    if (ctx->page == nullptr)
    {
        DCHECK(entry->vm_obj != nullptr);
        int st = vm_pf_get_page_from_vmo(ctx);
        if (st != VMO_STATUS_OK)
        {
            info->signal = find_page_err_to_signal(st);
            return -1;
        }
    }

    if (!map_pages_to_vaddr((void *) ctx->vpage, page_to_phys(ctx->page), PAGE_SIZE,
                            ctx->page_rwx | VM_NOFLUSH))
    {
        page_unpin(ctx->page);
        info->signal = VM_SIGSEGV;
        return -1;
    }

    page_unpin(ctx->page);

    return 0;
}

int vm_handle_write_wb(struct vm_pf_context *ctx)
{
    unsigned long paddr = MAPPING_INFO_PADDR(ctx->mapping_info);
    struct page *p = phys_to_page(paddr);
    int st = 0;
    struct inode *inode = p->owner->ino;
    st = vm_prepare_write(inode, p);
    if (st == 0)
    {
        paging_change_perms((void *) ctx->vpage, ctx->page_rwx);
        vm_invalidate_range(ctx->vpage, 1);
    }

    return st;
}

int vm_handle_present_pf(struct vm_pf_context *ctx)
{
    struct vm_area_struct *entry = ctx->entry;
    struct fault_info *info = ctx->info;
#if 0
	printk("Handling present PF at %lx %s%s%s\n", ctx->info->fault_address,
		ctx->info->write ? "W" : "-", ctx->info->read ? "R" : "-", ctx->info->exec ? "X" : "-");
#endif

    if (info->write & !(ctx->mapping_info & PAGE_WRITABLE))
    {
        if (vm_mapping_requires_wb(entry))
            return vm_handle_write_wb(ctx);
        panic("Strange case inside vm_handle_present_pf");
    }

    return 0;
}

int __vm_handle_pf(struct vm_area_struct *entry, struct fault_info *info)
{
    const pid_t pid = get_current_process()->pid_;
    const u64 addr = info->fault_address;
    const u8 fault_read = info->read;
    const u8 fault_write = info->write;
    const u8 fault_exec = info->exec;
    TRACE_EVENT_DURATION(vm_page_fault, addr, pid, fault_read, fault_write, fault_exec);
    struct vm_pf_context context;
    context.entry = entry;
    context.info = info;
    context.vpage = info->fault_address & -PAGE_SIZE;
    context.page = nullptr;
    context.page_rwx = entry->vm_flags;
    context.mapping_info = get_mapping_info((void *) context.vpage);

#if 0
	struct process *p = get_current_process();

	printk("fault on address %lx, page %lx, "
	  " present %s, process %d (%s)\n", context.info->fault_address,
	  context.vpage, context.mapping_info & PAGE_PRESENT ? "true" : "false",
	  p->pid, p->cmd_line);
#endif

    if (entry->vm_ops && entry->vm_ops->fault)
        return entry->vm_ops->fault(&context);

    if (context.mapping_info & PAGE_PRESENT)
    {
        if (vm_handle_present_pf(&context) < 0)
            return -1;
    }
    else
    {
        if (vm_handle_non_present_pf(&context) < 0)
            return -1;
    }

    // printk("elapsed: %lu ns\n", end - start);
    return 0;
}

/**
 * @brief Handles a page fault.
 *
 * @param info A pointer to a fault_info structure.
 * @return 0 on success or negative error codes.
 */
int vm_handle_page_fault(struct fault_info *info)
{
    bool use_kernel_as = !info->user && is_higher_half((void *) info->fault_address);
    struct mm_address_space *as =
        use_kernel_as ? &kernel_address_space : get_current_address_space();

    if (sched_is_preemption_disabled())
        panic("Page fault while preemption was disabled\n");
    if (irq_is_disabled())
        panic("Page fault while IRQs were disabled\n");

    /* Surrender immediately if there's no user address space or the fault was inside vm code */
    if (!as || mutex_holds_lock(&as->vm_lock))
    {
        info->signal = VM_SIGSEGV;
        return -1;
    }

    scoped_mutex g{as->vm_lock};

    struct vm_area_struct *entry = vm_find_region(as, (void *) info->fault_address);
    if (!entry)
    {
        struct thread *ct = get_current_thread();
        if (ct && info->user)
        {
            struct process *current = get_current_process();
            printk("Curr thread: %p\n", ct);
            const char *str;
            if (info->write)
                str = "write";
            else if (info->exec)
                str = "exec";
            else
                str = "read";
            printk("Page fault at %lx, %s, ip %lx, process name %s\n", info->fault_address, str,
                   info->ip, current ? current->cmd_line.c_str() : "(kernel)");
#if 0
            vm_print_umap();
            panic("pid %ld page fault", current->pid_);
#endif
        }

        info->signal = VM_SIGSEGV;
        return -1;
    }

    info->error_info = VM_BAD_PERMISSIONS;

    if (info->write && !(entry->vm_flags & VM_WRITE))
        return -1;
    if (info->exec && !(entry->vm_flags & VM_EXEC))
        return -1;
    if (info->user && !(entry->vm_flags & VM_USER))
        return -1;
    if (info->read && !(entry->vm_flags & VM_READ))
        return -1;

    info->error_info = 0;

    __sync_add_and_fetch(&as->page_faults, 1);

    int ret = __vm_handle_pf(entry, info);

    return ret;
}

static void vm_destroy_area(vm_area_struct *region)
{
    vm_mmu_unmap(region->vm_mm, (void *) region->vm_start, vma_pages(region));

    decrement_vm_stat(region->vm_mm, virtual_memory_size, region->vm_end - region->vm_start);

    if (is_mapping_shared(region))
    {
        decrement_vm_stat(region->vm_mm, shared_set_size, region->vm_end - region->vm_start);
    }

    vm_area_struct_destroy(region);
}

/**
 * @brief Destroys an address space.
 *
 * @param mm A pointer to a valid mm_address_space.
 */
void vm_destroy_addr_space(struct mm_address_space *mm)
{
    bool free_pgd = true;

    /* First, iterate through the rb tree and free/unmap stuff */
    scoped_mutex g{mm->vm_lock};

    vm_area_struct *entry;

    bst_for_every_entry_delete(&mm->region_tree, entry, vm_area_struct, vm_tree_node)
    {
        vm_destroy_area(entry);
    }

    assert(mm->resident_set_size == 0);
    assert(mm->shared_set_size == 0);
    assert(mm->virtual_memory_size == 0);
    assert(mm->page_tables_size == PAGE_SIZE);

    /* We're going to swap our address space to init's, and free our own */
    /* Note that we use mm explicitly, but switch to current->address_space explicitly.
     * This is because vm_destroy_addr_space is called when we need to destroy
     * an exec state (i.e an execve failure).
     */
    void *own_addrspace = vm_get_pgd(&mm->arch_mmu);

    if (own_addrspace == vm_get_fallback_pgd())
    {
        /* If init is deciding to exec without forking, don't free the fallback pgd! */
        free_pgd = false;
    }

    struct arch_mm_address_space old_arch_mmu;
    vm_set_pgd(&old_arch_mmu, own_addrspace);

    if (free_pgd)
        vm_free_arch_mmu(&old_arch_mmu);
}

/**
 * @brief Loads the fallback paging tables.
 *
 */
void vm_switch_to_fallback_pgd()
{
    assert(sched_is_preemption_disabled() == true);

    vm_load_arch_mmu(&kernel_address_space.arch_mmu);
}

/**
 * @brief Sanitises the address.
 * It does so by comparing it with a number of invalid ranges of
 * virtual memory, some of which may be arch dependent.
 * To be used by program loaders.
 *
 * @param address The address to-be-tested.
 * @param pages The size of the desired range.
 * @return 0 if it's valid, negative if not.
 */
int vm_sanitize_address(void *address, size_t pages)
{
    if (is_higher_half(address))
        return -1;
    if (is_invalid_arch_range(address, pages) < 0)
        return -1;
    return 0;
}

/**
 * @brief Generates a new mmap base, taking into account arch-dependent addresses and possibly
 * KASLR.
 *
 * @return The new mmap base. Note: This is not a valid pointer, but the starting point
 *         for mmap allocations.
 */
void *vm_gen_mmap_base()
{
    uintptr_t mmap_base = arch_mmap_base;
#ifdef CONFIG_ASLR
    if (enable_aslr)
    {
        mmap_base = vm_randomize_address(mmap_base, MMAP_ASLR_BITS);

        return (void *) mmap_base;
    }
#endif
    return (void *) mmap_base;
}

/**
 * @brief Generates a new brk base, taking into account arch-dependent addresses and possibly KASLR.
 *
 * @return The new brk base. Note: This is not a valid pointer, but the starting point
 *         for brk allocations.
 */
void *vm_gen_brk_base()
{
    uintptr_t brk_base = arch_brk_base;
#ifdef CONFIG_ASLR
    if (enable_aslr)
    {
        brk_base = vm_randomize_address(arch_brk_base, BRK_ASLR_BITS);
        return (void *) brk_base;
    }
#endif
    return (void *) brk_base;
}

int sys_memstat(struct memstat *memstat)
{
    struct memstat buf;
    page_get_stats(&buf);

    if (copy_to_user(memstat, &buf, sizeof(buf)) < 0)
        return -EFAULT;
    return 0;
}

/* Reads from vm_aslr - reads enable_aslr */
ssize_t aslr_read(void *buffer, size_t size, off_t off)
{
    UNUSED(size);
    UNUSED(off);
    char *buf = (char *) buffer;
    if (off == 1)
        return 0;

    if (enable_aslr)
    {
        *buf = '1';
    }
    else
        *buf = '0';
    return 1;
}

/* Writes to vm_aslr - modifies enable_aslr */
ssize_t aslr_write(void *buffer, size_t size, off_t off)
{
    UNUSED(size);
    UNUSED(off);
    char *buf = (char *) buffer;
    if (*buf == '1')
    {
        enable_aslr = true;
    }
    else if (*buf == '0')
    {
        enable_aslr = false;
    }

    return 1;
}

ssize_t vm_traverse_kmaps(void *node, char *address, size_t *size, off_t off)
{
    UNUSED(node);
    UNUSED(size);
    UNUSED(off);
    /* First write the lowest addresses, then the middle address, and then the higher addresses
     */
    strcpy(address, "unimplemented\n");
    return strlen(address);
}

ssize_t kmaps_read(void *buffer, size_t size, off_t off)
{
    UNUSED(off);
    return 0;
#if 0
	return vm_traverse_kmaps(kernel_tree, buffer, &size, 0);
#endif
}

ssize_t evict_write(void *buf, size_t size, off_t off)
{
    if (size == 0)
        return 0;
    char c;

    if (copy_from_user(&c, buf, 1) < 0)
        return -EFAULT;

    if (c == '2' || c == '3')
        dentry_trim_caches();

    if (c == '1' || c == '3')
        inode_trim_cache();

    return size;
}

static struct sysfs_object vm_obj;
static struct sysfs_object aslr_control;
static struct sysfs_object kmaps;
static struct sysfs_object evict_obj;

/**
 * @brief Initialises sysfs nodes for the vm subsystem.
 *
 */
void vm_sysfs_init(void)
{
    INFO("vmm", "Setting up /sys/vm\n");

    assert(sysfs_object_init("vm", &vm_obj) == 0);
    vm_obj.perms = 0755 | S_IFDIR;

    assert(sysfs_init_and_add("aslr_ctl", &aslr_control, &vm_obj) == 0);
    aslr_control.read = aslr_read;
    aslr_control.write = aslr_write;
    aslr_control.perms = 0644 | S_IFREG;

    assert(sysfs_init_and_add("kmaps", &kmaps, &vm_obj) == 0);
    kmaps.read = kmaps_read;
    kmaps.perms = 0444 | S_IFREG;

    assert(sysfs_init_and_add("evict", &evict_obj, &vm_obj) == 0);
    evict_obj.write = evict_write;
    evict_obj.perms = 0644 | S_IFREG;

    sysfs_add(&vm_obj, nullptr);
}

char *strcpy_from_user(const char *uptr)
{
    ssize_t len = strlen_user(uptr);
    if (len < 0)
    {
        errno = EFAULT;
        return nullptr;
    }

    char *buf = (char *) malloc(len + 1);
    if (!buf)
        return nullptr;
    buf[len] = '\0';

    if (copy_from_user(buf, uptr, len) < 0)
    {
        free(buf);
        errno = EFAULT;
        return nullptr;
    }

    return buf;
}

/**
 * @brief Updates the memory map's ranges.
 * Used in arch dependent early boot procedures when architectures
 * have variable address space sizes. See example uses of this
 * function in arch/x86_64.
 *
 * @param new_kernel_space_base The new virtual memory base.
 */
void vm_update_addresses(uintptr_t new_kernel_space_base)
{
    vmalloc_space += new_kernel_space_base;
    high_half = new_kernel_space_base;
}

/**
 * @brief Generate a ASLR'd address.
 * Takes into account base and bits.
 *
 * @param base The base address.
 * @param bits The number of bits that can be randomised.
 * @return The randomised address.
 */
uintptr_t vm_randomize_address(uintptr_t base, uintptr_t bits)
{
#ifdef CONFIG_KASLR
    if (bits != 0)
        bits--;
    uintptr_t mask = UINTPTR_MAX & ~(-(1UL << bits));
    /* Get entropy from arc4random() */
    uintptr_t result = ((uintptr_t) arc4random() << 12) & mask;
    result |= ((uintptr_t) arc4random() << 44) & mask;

    base |= result;
#endif
    return base;
}

/**
 * @brief Does the fatal page fault procedure.
 * When a user fault, kills the process; else, panics.
 *
 * @param info A pointer to a fault_info structure.
 */
void vm_do_fatal_page_fault(struct fault_info *info)
{
    bool is_user_mode = info->user;

    if (is_user_mode)
    {
        struct process *current = get_current_process();
        printf("%s at %016lx at ip %lx in process %u(%s)\n",
               info->signal == SIGSEGV ? "SEGV" : "SIGBUS", info->fault_address, info->ip,
               current->get_pid(), current->cmd_line.c_str());
        printf("Error info: %x on %c%c%c\n", info->error_info, info->read ? 'r' : '-',
               info->write ? 'w' : '-', info->exec ? 'x' : '-');
        printf("Program base: %p\n", current->interp_base);

        siginfo_t sinfo = {};

        if (info->signal == SIGSEGV)
        {
            sinfo.si_code = info->error_info & VM_BAD_PERMISSIONS ? SEGV_ACCERR : SEGV_MAPERR;
        }
        else if (info->signal == SIGBUS)
        {
            // TODO: Add si_codes for sigbus
            sinfo.si_code = SI_KERNEL;
        }

        sinfo.si_addr = (void *) info->fault_address;
        kernel_tkill(info->signal, get_current_thread(), SIGNAL_FORCE, &sinfo);
    }
    else
    {
        panic("Kernel fatal segfault accessing %016lx at ip %lx\n", info->fault_address, info->ip);
    }
}

/**
 * @brief Sets up backing for a newly-mmaped region.
 *
 * @param region A pointer to a vm_area_struct.
 * @param pages The size of the region, in pages.
 * @param is_file_backed True if file backed.
 * @return 0 on success, negative for errors.
 */
int vm_area_struct_setup_backing(struct vm_area_struct *region, size_t pages, bool is_file_backed)
{
    bool is_shared = is_mapping_shared(region);

    if (!is_file_backed && is_shared)
    {
        region->vm_file = anon_get_shmem(pages << PAGE_SHIFT);
        if (!region->vm_file)
            return -ENOMEM;
        is_file_backed = true;
    }

    struct vm_object *vmo;

    if (is_file_backed)
    {
        struct inode *ino = region->vm_file->f_ino;

        assert(ino->i_pages != nullptr);
        vmo_ref(ino->i_pages);
        vmo = ino->i_pages;

        if (!is_shared)
            region->vm_ops = &private_vmops;
    }
    else
    {
        /* Anonymous, private memory uses amaps now */
        vmo = nullptr;
        region->vm_ops = &anon_vmops;
    }

    if (vmo)
    {
        vmo_assign_mapping(vmo, region);
        assert(region->vm_obj == nullptr);
        region->vm_obj = vmo;
    }

    return 0;
}

/**
 * @brief Determines if a mapping is shared.
 *
 * @param region A pointer to the vm_area_struct.
 * @return True if shared, false if not.
 */
bool is_mapping_shared(struct vm_area_struct *region)
{
    return region->vm_maptype == MAP_SHARED;
}

/**
 * @brief Determines if a mapping is file backed.
 *
 * @param region A pointer to the vm_area_struct.
 * @return True if file backed, false if not.
 */
bool is_file_backed(struct vm_area_struct *region)
{
    return region->vm_file != nullptr;
}

/**
 * @brief Allocates a new mapping and maps a list of pages.
 *
 * @param pl The list of pages.
 * @param size The size of the mapping, in bytes.
 * @param prot The desired protection flags.
 * @return A pointer to the new mapping, or NULL if it failed.
 */
void *map_page_list(struct page *pl, size_t size, uint64_t prot)
    EXCLUDES(kernel_address_space.vm_lock)
{
    // TODO: Maybe also use vmalloc for this?
    scoped_mutex g{kernel_address_space.vm_lock};
    struct vm_area_struct *entry = __vm_allocate_virt_region(
        &kernel_address_space, VM_KERNEL, vm_size_to_pages(size), VM_TYPE_REGULAR, prot);
    if (!entry)
        return nullptr;
    void *vaddr = (void *) entry->vm_start;

    uintptr_t u = (uintptr_t) vaddr;
    while (pl != nullptr)
    {
        if (!map_pages_to_vaddr((void *) u, page_to_phys(pl), PAGE_SIZE, prot))
        {
            __vm_munmap(&kernel_address_space, vaddr, size);
            return nullptr;
        }

        pl = pl->next_un.next_allocation;
        u += PAGE_SIZE;
    }

    return vaddr;
}

/**
 * @brief Sets up a new address space on \p mm
 *
 * @param mm A pointer to the new address space.
 * @return 0 on success, negative error codes.
 */
int vm_create_address_space(struct mm_address_space *mm)
{
    mm->mmap_base = vm_gen_mmap_base();
    mm->start = arch_low_half_min;
    mm->end = arch_low_half_max;
    mm->resident_set_size = 0;
    mm->shared_set_size = 0;
    mm->virtual_memory_size = 0;

    assert(mm->active_mask.is_empty() == true);

    mutex_init(&mm->vm_lock);

    bst_root_initialize(&mm->region_tree);

    return 0;
}

/**
 * @brief Sets up the brk region for a new process.
 *
 * @param mm The target address space.
 * @return 0 on success, or negative error codes.
 */
int vm_create_brk(struct mm_address_space *mm)
{
    mm->brk = vm_mmap(vm_gen_brk_base(), 1 << PAGE_SHIFT, PROT_WRITE,
                      MAP_PRIVATE | MAP_FIXED | MAP_ANON, nullptr, 0);

    if (!mm->brk)
    {
        return -ENOMEM;
    }

    return 0;
}

/**
 * @brief Retrieves the fallback paging directories.
 * The kernel has a fallback pgd on which process fall back to right before freeing
 * its own pgd, during process destruction.
 *
 * @return void* The fallback pgd.
 */
void *vm_get_fallback_pgd()
{
    return vm_get_pgd(&kernel_address_space.arch_mmu);
}

void vm_remove_region(struct mm_address_space *as, struct vm_area_struct *region)
    REQUIRES(as->vm_lock)
{
    MUST_HOLD_MUTEX(&as->vm_lock);

    bst_delete(&as->region_tree, &region->vm_tree_node);
}

int vm_add_region(struct mm_address_space *as, struct vm_area_struct *region)
{
    MUST_HOLD_MUTEX(&as->vm_lock);

    return vm_insert_region(as, region);
}

int __vm_munmap(struct mm_address_space *as, void *__addr, size_t size) REQUIRES(as->vm_lock)
{
    unsigned long addr = (unsigned long) __addr & -PAGE_SIZE;
    unsigned long limit = ALIGN_TO(((unsigned long) __addr) + size, PAGE_SIZE);
    size = limit - addr;
    struct list_head list = LIST_HEAD_INIT(list);

    MUST_HOLD_MUTEX(&as->vm_lock);

    struct vm_area_struct *region = vm_search(as, (void *) addr, PAGE_SIZE);
    if (!region)
        return -EINVAL;

    /* Split the head, if needed */
    if (addr > region->vm_start)
    {
        region = vm_split_region(as, region, addr, false);
        if (!region)
            return -ENOMEM;
        DCHECK(addr == region->vm_start);
    }

    /* Gather munmap regions into our local list. No permanent changes are done in this loop,
     * while regions are live *except unlinking from the BST*.
     */

    while (region)
    {
        /* Split the tail, if needed */
        if (limit < region->vm_end)
        {
            region = vm_split_region(as, region, limit, true);
            if (!region)
                goto restore;
            DCHECK(limit == region->vm_end);
        }

        addr = region->vm_end;

        struct vm_area_struct *next = containerof_null_safe(
            bst_next(&as->region_tree, &region->vm_tree_node), struct vm_area_struct, vm_tree_node);
        /* Note: vm_detached_node and tree_node alias each other, so we need to remove first, add
         * later.
         */
        vm_remove_region(as, region);
        list_add_tail(&region->vm_detached_node, &list);

        if (limit == region->vm_end)
            break;
        region = next;
    }

    list_for_every_safe (&list)
    {
        struct vm_area_struct *vma = container_of(l, struct vm_area_struct, vm_tree_node);
        bool is_shared = is_mapping_shared(vma);
        unsigned long sz = vma->vm_end - vma->vm_start;

        vm_mmu_unmap(as, (void *) vma->vm_start, vma_pages(vma));
        list_remove(&vma->vm_detached_node);
        vm_area_struct_destroy(vma);

        decrement_vm_stat(as, virtual_memory_size, sz);
        if (is_shared)
            decrement_vm_stat(as, shared_set_size, sz);
    }

    return 0;
restore:
    /* Something back there just failed, restore the old regions and -ENOMEM */
    list_for_every_safe (&list)
    {
        struct vm_area_struct *vma = container_of(l, struct vm_area_struct, vm_tree_node);
        list_remove(&vma->vm_detached_node);
        bst_node_initialize(&vma->vm_tree_node);
        vm_insert_region(as, vma);
    }

    return -ENOMEM;
}

/**
 * @brief Unmaps a memory range.
 *
 * @param as The target address space.
 * @param __addr The start of the memory range.
 * @param size The size of the memory range, in bytes.
 * @return 0 on success, or negative error codes.
 */
int vm_munmap(struct mm_address_space *as, void *__addr, size_t size)
{
    scoped_mutex g{as->vm_lock};

    auto addr = (unsigned long) __addr;
    if (addr < as->start || addr > as->end)
        return -EINVAL;
    if (size == 0)
        return -EINVAL;
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;

    return __vm_munmap(as, __addr, size);
}

#if CONFIG_TRACK_TLB_DELTA
hrtime_delta_t last_inval_delta = 0;
#endif

/**
 * @brief Invalidates a range of memory in the current address space.
 * This function handles TLB shootdowns on its own.
 *
 * @param addr The start of the range.
 * @param pages The size of the range, in pages.
 */
void vm_invalidate_range(unsigned long addr, size_t pages)
{
    return mmu_invalidate_range(addr, pages, get_current_address_space());
}

struct vm_area_struct *vm_next_region(mm_address_space *as, vm_area_struct *region)
{
    auto node = bst_next(&as->region_tree, &region->vm_tree_node);

    return node ? container_of(node, vm_area_struct, vm_tree_node) : nullptr;
}

bool vm_can_expand(struct mm_address_space *as, struct vm_area_struct *region, size_t new_size)
{
    /* Can always shrink the mapping */
    if (new_size < region->vm_end - region->vm_start)
        return true;

    auto next = vm_next_region(as, region);

    // If there's no region after this one, we're clear to expand
    // TODO: What if we overflow here?
    if (!next)
        return true;

    /* Calculate the hole size, and if >= new_size, we're good */
    size_t hole_size = next->vm_start - region->vm_start;

    return hole_size >= new_size;
}

int __vm_expand_mapping(struct vm_area_struct *region, size_t new_size)
{
    size_t diff = new_size - (region->vm_end - region->vm_start);
    if (!vm_test_vs_rlimit(region->vm_mm, new_size))
        return -ENOMEM;

    region->vm_end += diff;
    increment_vm_stat(region->vm_mm, virtual_memory_size, diff);
    if (is_mapping_shared(region))
        increment_vm_stat(region->vm_mm, shared_set_size, diff);

    return 0;
}

int vm_expand_mapping(struct mm_address_space *as, struct vm_area_struct *region, size_t new_size)
    REQUIRES(as->vm_lock)
{
    MUST_HOLD_MUTEX(&as->vm_lock);

    if (!vm_can_expand(as, region, new_size))
    {
        return -1;
    }

    return __vm_expand_mapping(region, new_size);
}

int vm_expand_brk(struct mm_address_space *as, size_t nr_pages) REQUIRES(as->vm_lock)
{
    struct vm_area_struct *brk_region = vm_find_region(as, as->brk);
    assert(brk_region != nullptr);
    size_t new_size = (vma_pages(brk_region) + nr_pages) << PAGE_SHIFT;

    return vm_expand_mapping(as, brk_region, new_size);
}

int mremap_check_for_overlap(void *__old_address, size_t old_size, void *__new_address,
                             size_t new_size)
{
    unsigned long old_address = (unsigned long) __old_address;
    unsigned long new_address = (unsigned long) __new_address;

    /* Written at 03:00, but the logic looks good? */
    if (old_address <= (unsigned long) new_address &&
        old_address + old_size > (unsigned long) new_address)
        return -1;
    if (old_address <= (unsigned long) new_address + new_size &&
        old_address + old_size > (unsigned long) new_address + new_size)
        return -1;
    return 0;
}

void *vm_remap_create_new_mapping_of_shared_pages(struct mm_address_space *mm, void *new_address,
                                                  size_t new_size, int flags, void *old_address)
    REQUIRES(mm->vm_lock)
{
    void *ret = MAP_FAILED;
    bool fixed = flags & MREMAP_FIXED;
    struct vm_area_struct *new_mapping = nullptr;

    struct vm_area_struct *old_region = vm_find_region(mm, old_address);
    if (!old_region)
    {
        ret = (void *) -EFAULT;
        goto out;
    }

    if (old_region->vm_maptype != MAP_SHARED)
    {
        ret = (void *) -EINVAL;
        goto out;
    }

    if (fixed)
    {
        if (vm_sanitize_address(new_address, new_size >> PAGE_SHIFT) < 0)
        {
            ret = (void *) -EINVAL;
            goto out;
        }

        if (mremap_check_for_overlap(old_address, new_size, new_address, new_size) < 0)
        {
            ret = (void *) -EINVAL;
            goto out;
        }

        new_mapping = __vm_create_region_at(mm, new_address, new_size >> PAGE_SHIFT,
                                            VM_TYPE_REGULAR, old_region->vm_flags);
    }
    else
    {
        new_mapping =
            vm_allocate_region(mm, (unsigned long) mm->mmap_base, new_size, VM_ADDRESS_USER);
        if (new_mapping)
        {
            new_mapping->vm_flags = old_region->vm_flags;
        }
    }

    if (!new_mapping)
    {
        ret = (void *) -ENOMEM;
        goto out;
    }

    vm_copy_region(old_region, new_mapping);
    ret = (void *) new_mapping->vm_start;
out:
    return ret;
}

void *vm_try_move(struct mm_address_space *mm, struct vm_area_struct *old_region,
                  unsigned long new_base, size_t new_size) REQUIRES(mm->vm_lock)
{
    vm_remove_region(mm, old_region);

    old_region->vm_start = new_base;
    if (int st = __vm_expand_mapping(old_region, new_size); st < 0)
        return (void *) (unsigned long) st;

    /* TODO: What to do in case of a failure? */
    vm_add_region(mm, old_region);

    /* TODO: Maybe unmapping isn't the best option on a move and we should copy mappings */
    __vm_unmap_range(mm, (void *) old_region->vm_start, vma_pages(old_region));

    vm_print_umap();
    return (void *) old_region->vm_start;
}

void *vm_remap_try(struct mm_address_space *as, void *old_address, size_t old_size,
                   void *new_address, size_t new_size, int flags) REQUIRES(as->vm_lock)
{
    struct vm_area_struct *reg = vm_find_region(as, old_address);
    if (!reg)
        return (void *) -EFAULT;
#if 0
    struct vm_area_struct *old_reg = vm_split_region(as, reg, (unsigned long) old_address, old_size, &n);
    if (!old_reg)
        return (void *) -ENOMEM;

    if (vm_expand_mapping(as, old_reg, new_size) < 0)
    {
        if (flags & MREMAP_MAYMOVE)
        {
            unsigned long new_base =
                vm_allocate_base(as, (unsigned long) as->mmap_base, new_size, VM_ADDRESS_USER);
            return vm_try_move(as, old_reg, new_base, new_size);
        }

        return (void *) -ENOMEM;
    }

    return (void *) old_reg->base;
#endif
    return (void *) -ENOMEM;
}

bool limits_are_contained(struct vm_area_struct *reg, unsigned long start, unsigned long limit)
{
    unsigned long reg_limit = reg->vm_end;

    if (start <= reg->vm_start && limit > reg->vm_start)
        return true;
    if (reg->vm_start <= start && reg_limit >= limit)
        return true;

    return false;
}

/* TODO: Test things */
void *sys_mremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address)
{
    // TODO: This is broken.
    return (void *) -ENOSYS;
    /* Check http://man7.org/linux/man-pages/man2/mremap.2.html for documentation */
    struct process *current = get_current_process();
    bool may_move = flags & MREMAP_MAYMOVE;
    bool fixed = flags & MREMAP_FIXED;
    bool wants_create_new_mapping_of_pages = old_size == 0 && may_move;
    void *ret = MAP_FAILED;
    scoped_mutex g{current->address_space->vm_lock};
    auto as = current->get_aspace();

    /* TODO: Unsure on what to do if new_size > old_size */

    if (vm_sanitize_address(old_address, old_size >> PAGE_SHIFT) < 0)
    {
        ret = (void *) -EFAULT;
        goto out;
    }

    if (wants_create_new_mapping_of_pages)
        return vm_remap_create_new_mapping_of_shared_pages(as, new_address, new_size, flags,
                                                           old_address);

    if (old_size == 0)
    {
        ret = (void *) -EINVAL;
        goto out;
    }

    if (new_size == 0)
    {
        ret = (void *) -EINVAL;
        goto out;
    }

    if (!fixed)
    {
        ret = vm_remap_try(as, old_address, old_size, new_address, new_size, flags);
        goto out;
    }
    else
    {

        if (vm_sanitize_address(new_address, new_size >> PAGE_SHIFT) < 0)
        {
            ret = (void *) -EINVAL;
            goto out;
        }

        if (mremap_check_for_overlap(old_address, old_size, new_address, new_size) < 0)
        {
            ret = (void *) -EINVAL;
            goto out;
        }

        struct vm_area_struct *reg = vm_find_region(as, old_address);
        if (!reg)
        {
            ret = (void *) -EFAULT;
            goto out;
        }
#if 0
        struct vm_area_struct *old_reg =
            vm_split_region(as, reg, (unsigned long) old_address, old_size, &n);
        if (!old_reg)
        {
            ret = (void *) -ENOMEM;
            goto out;
        }

        __vm_munmap(as, new_address, new_size);

        ret = vm_try_move(as, old_reg, (unsigned long) new_address, new_size);
#endif
    }

out:
    return ret;
}

void vm_wp_page(struct mm_address_space *mm, void *vaddr)
{
    if (paging_write_protect(vaddr, mm))
        mmu_invalidate_range((unsigned long) vaddr, 1, mm);
}

int get_phys_pages_direct(unsigned long addr, unsigned int flags, struct page **pages,
                          size_t nr_pgs)
{
    if (flags & GPP_USER)
        return GPP_ACCESS_FAULT;

    /* This is a PFNMAP kind of thing, so we don't have pages to reference.
     * We'll just need to pretend each paddr is a page struct and tell the caller at the
     * end, using GPP_ACCESS_PFNMAP.
     */

    for (size_t i = 0; i < nr_pgs; i++, addr += PAGE_SIZE)
    {
        unsigned long paddr = addr - PHYS_BASE;
        struct page *p = phys_to_page(paddr);
        pages[i] = p;
        page_ref(p);
    }

    return GPP_ACCESS_OK | GPP_ACCESS_PFNMAP;
}

int gpp_try_to_fault_in(unsigned long addr, struct vm_area_struct *entry, unsigned int flags)
{
    struct fault_info finfo;
    finfo.signal = 0;
    finfo.exec = false;
    finfo.fault_address = addr;
    finfo.ip = 0;
    finfo.read = flags & GPP_READ;
    finfo.user = true;
    finfo.write = flags & GPP_WRITE;

    if (__vm_handle_pf(entry, &finfo) < 0)
    {
        return GPP_ACCESS_FAULT;
    }

    return GPP_ACCESS_OK;
}

int __get_phys_pages(struct vm_area_struct *region, unsigned long addr, unsigned int flags,
                     struct page **pages, size_t nr_pgs)
{
    unsigned long page_rwx_mask = (flags & GPP_READ ? PAGE_PRESENT : 0) |
                                  (flags & GPP_WRITE ? PAGE_WRITABLE : 0) |
                                  (flags & GPP_USER ? PAGE_USER : 0);

    for (size_t i = 0; i < nr_pgs; i++, addr += PAGE_SIZE)
    {
    retry:;
        unsigned long mapping_info = get_mapping_info((void *) addr);

        if ((mapping_info & page_rwx_mask) != page_rwx_mask)
        {
            int st = gpp_try_to_fault_in(addr, region, flags);

            if (!(st & GPP_ACCESS_OK))
                return st;
            goto retry;
        }

        unsigned long paddr = MAPPING_INFO_PADDR(mapping_info);

        struct page *page = phys_to_page(paddr);

        pages[i] = page;
    }

    return GPP_ACCESS_OK;
}

/**
 * @brief Gets the physical pages that map to a virtual region.
 * This function also handles COW.
 *
 * @param addr The desired virtual address.
 * @param flags Flags (see GPP_READ, WRITE and USER).
 * @param pages A pointer to an array of struct page *.
 * @param nr The number of pages of the virtual address range.
 * @return A bitmask of GPP_ACCESS_* flags; see the documentation for those
 *         for more information.
 */
int get_phys_pages(void *_addr, unsigned int flags, struct page **pages, size_t nr_pgs)
{
    bool is_user = flags & GPP_USER;
    int ret = GPP_ACCESS_OK;
    bool had_shared_pages = false;
    size_t number_of_pages = nr_pgs;

    struct mm_address_space *as = is_user ? get_current_address_space() : &kernel_address_space;

    unsigned long addr = (unsigned long) _addr;

    if (addr >= PHYS_BASE && (addr + (nr_pgs << PAGE_SHIFT)) < PHYS_BASE_LIMIT)
    {
        ret = get_phys_pages_direct(addr, flags, pages, nr_pgs);
        return ret;
    }

    scoped_mutex g{as->vm_lock};

    size_t pages_gotten = 0;

    while (nr_pgs)
    {
        struct vm_area_struct *reg = vm_find_region(as, (void *) addr);

        if (!reg)
        {
            ret = GPP_ACCESS_FAULT;
            goto out;
        }

        if (reg->vm_maptype == MAP_SHARED)
            had_shared_pages = true;

        /* Do a permission check. */
        unsigned int rwx_mask = (flags & GPP_READ ? VM_READ : 0) |
                                (flags & GPP_WRITE ? VM_WRITE : 0) |
                                (flags & GPP_USER ? VM_USER : 0);

        if ((reg->vm_flags & rwx_mask) != rwx_mask)
        {
            ret = GPP_ACCESS_FAULT;
            goto out;
        }

        /* Calculate the number of pages we can resolve in this region */
        size_t vm_area_struct_off_pgs = (reg->vm_start - addr) >> PAGE_SHIFT;
        size_t max_resolved_pgs = reg->vm_end - reg->vm_start - vm_area_struct_off_pgs;
        size_t resolved_pgs = min(nr_pgs, max_resolved_pgs);

        /* And now resolve stuff */
        ret = __get_phys_pages(reg, addr, flags, pages + pages_gotten, resolved_pgs);

        /* Bail if we've hit an error */
        if (!(ret & GPP_ACCESS_OK))
            goto out;

        nr_pgs -= resolved_pgs;
        pages_gotten += resolved_pgs;
        addr += nr_pgs << PAGE_SHIFT;
    }

    /* Now that we're done, we're pinning the pages we just got */

    for (size_t i = 0; i < number_of_pages; i++)
        page_pin(pages[i]);

out:
    if (ret & GPP_ACCESS_OK && had_shared_pages)
        ret |= GPP_ACCESS_SHARED;

    return ret;
}

static int do_sync(struct file *file, unsigned long start, unsigned long end)
{
    struct writepages_info wp;
    wp.start = start >> PAGE_SHIFT;
    wp.end = (end - 1) >> PAGE_SHIFT;
    wp.flags = WRITEPAGES_SYNC;
    if (file->f_ino->i_fops->fsyncdata)
        return file->f_ino->i_fops->fsyncdata(file->f_ino, &wp);
    return -EINVAL;
}

/**
 * @brief Retrieves a pointer to the zero page.
 *
 * @return Pointer to the zero page's struct page.
 */
struct page *vm_get_zero_page()
{
    return vm_zero_page;
}

int sys_msync(void *ptr, size_t length, int flags)
{
    /* The only flag that needs action upon is MS_SYNC, the rest is a no-op. */
    int st = -ENOMEM;
    unsigned long addr = (unsigned long) ptr;
    unsigned long limit = addr + length;
    struct mm_address_space *as = get_current_address_space();

    if (flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE))
        return -EINVAL;

    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;

    /* Hogging the vm_lock is bad mkay, todo... */
    scoped_mutex g{as->vm_lock};

    struct vm_area_struct *vma = vm_search(as, (void *) addr, PAGE_SIZE);

    if (vma)
    {
        /* Check if start <= addr */
        if (vma->vm_start > addr)
            return -ENOMEM;
    }

    while (vma)
    {
        if (vma->vm_start > limit || vma->vm_end < addr)
            break;
        unsigned long to_sync = cul::min(length, vma->vm_end - addr);
        struct file *filp = vma->vm_file;

        if (flags & MS_SYNC && filp && is_mapping_shared(vma))
        {
            unsigned long start = vma->vm_offset + addr - vma->vm_start;
            unsigned long end = start + to_sync;
            int st2 = do_sync(filp, start, end);
            if (st2 < 0)
            {
                st = st2;
                break;
            }
        }

        addr += to_sync;
        length -= to_sync;
        vma = containerof_null_safe(bst_next(&as->region_tree, &vma->vm_tree_node),
                                    struct vm_area_struct, vm_tree_node);
        if (!length)
        {
            st = 0;
            break;
        }

        if (vma && vma->vm_start != addr)
            break;
    }

    return st;
}

/**
 * @brief Creates a new standalone address space
 *
 * @return Ref guard to a mm_address_space, or a negative status code
 */
expected<ref_guard<mm_address_space>, int> mm_address_space::create()
{
    ref_guard<mm_address_space> as = make_refc<mm_address_space>();
    if (!as)
        return unexpected<int>{-ENOENT};

    spinlock_init(&as->page_table_lock);
    bst_root_initialize(&as->region_tree);

    int st = vm_clone_as(as.get());
    if (st < 0)
        return unexpected<int>{st};
    return as;
}

/**
 * @brief Creates a new standalone address space by forking
 *
 * @return Ref guard to a mm_address_space, or a negative status code
 */
expected<ref_guard<mm_address_space>, int> mm_address_space::fork()
{
    TRACE_EVENT_DURATION(vm_fork_mm);
    ref_guard<mm_address_space> as = make_refc<mm_address_space>();
    if (!as)
        return unexpected<int>{-ENOENT};

    spinlock_init(&as->page_table_lock);
    bst_root_initialize(&as->region_tree);

    int st = vm_fork_address_space(as.get());
    if (st < 0)
        return unexpected<int>{st};
    return as;
}

/**
 * @brief Loads an address space
 *
 * @param aspace Address space to load
 * @param cpu CPU we're on
 */
void vm_load_aspace(mm_address_space *aspace, unsigned int cpu)
{
    vm_load_arch_mmu(&aspace->arch_mmu);
    if (cpu == -1U) [[unlikely]]
        cpu = get_cpu_nr();
    aspace->active_mask.set_cpu_atomic(cpu);
}

/**
 * @brief Sets the current address space, and returns the old one
 *
 * @param aspace Address space to set and load
 * @return The old address space
 */
mm_address_space *vm_set_aspace(mm_address_space *aspace)
{
    mm_address_space *ret = &kernel_address_space;
    auto thread = get_current_thread();
    if (thread)
    {
        ret = thread->get_aspace();
        thread->set_aspace(aspace);
    }

    vm_load_aspace(aspace, -1);

    return ret;
}

/**
 * @brief Destroys the mm_address_space object
 *
 */
mm_address_space::~mm_address_space()
{
    vm_destroy_addr_space(this);
}
