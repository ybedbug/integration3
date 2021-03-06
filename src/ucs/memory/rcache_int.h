/**
 * Copyright (C) Mellanox Technologies Ltd. 2018.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCS_REG_CACHE_INT_H_
#define UCS_REG_CACHE_INT_H_

#include "rcache.h"

#include <ucs/datastruct/list.h>
#include <ucs/datastruct/queue.h>
#include <ucs/stats/stats.h>
#include <ucs/type/spinlock.h>


/* Names of rcache stats counters */
enum {
    UCS_RCACHE_GETS,                /* number of get operations */
    UCS_RCACHE_HITS_FAST,           /* number of fast path hits */
    UCS_RCACHE_HITS_SLOW,           /* number of slow path hits */
    UCS_RCACHE_MISSES,              /* number of misses */
    UCS_RCACHE_MERGES,              /* number of region merges */
    UCS_RCACHE_UNMAPS,              /* number of memory unmap events */
    UCS_RCACHE_UNMAP_INVALIDATES,   /* number of regions invalidated because
                                       of unmap events */
    UCS_RCACHE_PUTS,                /* number of put operations */
    UCS_RCACHE_REGS,                /* number of memory registrations */
    UCS_RCACHE_DEREGS,              /* number of memory deregistrations */
    UCS_RCACHE_STAT_LAST
};


struct ucs_rcache {
    ucs_rcache_params_t      params;   /**< rcache parameters (immutable) */
    pthread_rwlock_t         lock;     /**< Protects the page table and all regions
                                            whose refcount is 0 */
    ucs_pgtable_t            pgtable;  /**< page table to hold the regions */

    ucs_recursive_spinlock_t inv_lock; /**< Lock for inv_q and inv_mp. This is a
                                          separate lock because we may want to put
                                          regions on inv_q while the page table
                                          lock is held by the calling context */
    ucs_queue_head_t         inv_q;    /**< Regions which were invalidated during
                                            memory events */
    ucs_mpool_t              inv_mp;   /**< Memory pool to allocate entries for inv_q,
                                            since we cannot use regulat malloc().
                                            The backing storage is original mmap()
                                            which does not generate memory events */
    unsigned long            num_regions;/**< Total number of managed regions */
    size_t                   total_size; /**< Total size of registered memory */
    unsigned long            num_evictions; /**< Total number of evictions */

    struct {
        ucs_spinlock_t       lock;     /**< Lock for this structure */
        ucs_list_link_t      list;     /**< List of regions, sorted by usage:
                                            The head of the list is the least
                                            recently used region, and the tail
                                            is the most recently used region. */
        unsigned long        count;    /**< Number of regions on list */
    } lru;

    char                     *name;    /**< Name for debug purposes */

    UCS_STATS_NODE_DECLARE(stats)
};


void ucs_rcache_check_inv_queue_slow(ucs_rcache_t *rcache);


static UCS_F_ALWAYS_INLINE void
ucs_rcache_check_inv_queue_fast(ucs_rcache_t *rcache)
{
    if (ucs_unlikely(!ucs_queue_is_empty(&rcache->inv_q))) {
        ucs_rcache_check_inv_queue_slow(rcache);
    }
}

#endif
