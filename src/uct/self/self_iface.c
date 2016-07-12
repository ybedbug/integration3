/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2016.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include "self_iface.h"
#include "self_md.h"
#include "self_ep.h"

#include <uct/base/uct_ep.h>
#include <ucs/type/class.h>

static ucs_config_field_t uct_self_iface_config_table[] = {
    {"", "", NULL,
     ucs_offsetof(uct_self_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {NULL}
};

static ucs_status_t uct_self_iface_query(uct_iface_h iface, uct_iface_attr_t *attr)
{
    uct_self_iface_t *self_iface = ucs_derived_of(iface, uct_self_iface_t);

    ucs_trace_func("iface=%p", iface);
    memset(attr, 0, sizeof(*attr));

    attr->iface_addr_len         = sizeof(uct_self_iface_addr_t);
    attr->device_addr_len        = 0;
    attr->ep_addr_len            = 0; /* No UCT_IFACE_FLAG_CONNECT_TO_EP supported */
    attr->cap.flags              = UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                   UCT_IFACE_FLAG_AM_SHORT         |
                                   UCT_IFACE_FLAG_AM_BCOPY         |
                                   UCT_IFACE_FLAG_PUT_SHORT        |
                                   UCT_IFACE_FLAG_PUT_BCOPY        |
                                   UCT_IFACE_FLAG_GET_BCOPY        |
                                   UCT_IFACE_FLAG_ATOMIC_ADD32     |
                                   UCT_IFACE_FLAG_ATOMIC_ADD64     |
                                   UCT_IFACE_FLAG_ATOMIC_FADD64    |
                                   UCT_IFACE_FLAG_ATOMIC_FADD32    |
                                   UCT_IFACE_FLAG_ATOMIC_SWAP64    |
                                   UCT_IFACE_FLAG_ATOMIC_SWAP32    |
                                   UCT_IFACE_FLAG_ATOMIC_CSWAP64   |
                                   UCT_IFACE_FLAG_ATOMIC_CSWAP32   |
                                   UCT_IFACE_FLAG_PENDING          |
                                   UCT_IFACE_FLAG_AM_CB_SYNC;

    attr->cap.put.max_short      = UINT_MAX;
    attr->cap.put.max_bcopy      = SIZE_MAX;
    attr->cap.put.max_zcopy      = 0;

    attr->cap.get.max_bcopy      = SIZE_MAX;
    attr->cap.get.max_zcopy      = 0;

    attr->cap.am.max_short       = self_iface->data_length;
    attr->cap.am.max_bcopy       = self_iface->data_length;
    attr->cap.am.max_zcopy       = 0;
    attr->cap.am.max_hdr         = 0;

    attr->latency                = 0;
    attr->bandwidth              = 6911 * 1024.0 * 1024.0;
    attr->overhead               = 0;

    return UCS_OK;
}

static ucs_status_t uct_self_iface_get_address(uct_iface_h iface,
                                               uct_iface_addr_t *addr)
{
    const uct_self_iface_t *self_iface = 0;

    ucs_trace_func("iface=%p", iface);
    self_iface = ucs_derived_of(iface, uct_self_iface_t);
    *(uct_self_iface_addr_t*)addr = self_iface->id;
    return UCS_OK;
}

static int uct_self_iface_is_reachable(uct_iface_h iface,
                                       const uct_device_addr_t *addr)
{
    const uct_self_iface_t *self_iface = 0;

    self_iface = ucs_derived_of(iface, uct_self_iface_t);
    ucs_trace_func("iface=%p id=%lx addr=%lx",
                   iface, self_iface->id, *(uct_self_iface_addr_t*)addr);
    return  self_iface->id == *(const uct_self_iface_addr_t*)addr;
}

static void uct_self_iface_release_am_desc(uct_iface_t *tl_iface, void *desc)
{
    uct_am_recv_desc_t *self_desc = 0;

    self_desc = (uct_am_recv_desc_t *) desc - 1;
    ucs_trace_func("iface=%p, desc=%p", tl_iface, self_desc);
    ucs_mpool_put(self_desc);
}

static UCS_CLASS_DEFINE_DELETE_FUNC(uct_self_iface_t, uct_iface_t);

static uct_iface_ops_t uct_self_iface_ops = {
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_self_iface_t),
    .iface_get_device_address = ucs_empty_function_return_success,
    .iface_get_address        = uct_self_iface_get_address,
    .iface_query              = uct_self_iface_query,
    .iface_is_reachable       = uct_self_iface_is_reachable,
    .iface_release_am_desc    = uct_self_iface_release_am_desc,
    .ep_create_connected      = UCS_CLASS_NEW_FUNC_NAME(uct_self_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_self_ep_t),
    .ep_am_short              = uct_self_ep_am_short,
    .ep_am_bcopy              = uct_self_ep_am_bcopy,
    .ep_put_short             = uct_base_ep_put_short,
    .ep_put_bcopy             = uct_base_ep_put_bcopy,
    .ep_get_bcopy             = uct_base_ep_get_bcopy,
    .ep_atomic_add64          = uct_base_ep_atomic_add64,
    .ep_atomic_fadd64         = uct_base_ep_atomic_fadd64,
    .ep_atomic_cswap64        = uct_base_ep_atomic_cswap64,
    .ep_atomic_swap64         = uct_base_ep_atomic_swap64,
    .ep_atomic_add32          = uct_base_ep_atomic_add32,
    .ep_atomic_fadd32         = uct_base_ep_atomic_fadd32,
    .ep_atomic_cswap32        = uct_base_ep_atomic_cswap32,
    .ep_atomic_swap32         = uct_base_ep_atomic_swap32,
    .ep_pending_add           = ucs_empty_function_return_busy,
    .ep_pending_purge         = ucs_empty_function,
};

static ucs_mpool_ops_t ops = {
   ucs_mpool_chunk_malloc,
   ucs_mpool_chunk_free,
   NULL,
   NULL
};

static UCS_CLASS_INIT_FUNC(uct_self_iface_t, uct_md_h md, uct_worker_h worker,
                           const char *dev_name, size_t rx_headroom,
                           const uct_iface_config_t *tl_config)
{
    ucs_status_t status;
    uct_self_iface_config_t *self_config = 0;

    ucs_trace_func("Creating a loop-back transport self=%p rxh=%lu",
                   self, rx_headroom);

    if (strcmp(dev_name, UCT_SELF_NAME) != 0) {
        ucs_error("No device was found: %s", dev_name);
        return UCS_ERR_NO_DEVICE;
    }

    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t, &uct_self_iface_ops, md, worker,
                              tl_config UCS_STATS_ARG(NULL));

    self_config = ucs_derived_of(tl_config, uct_self_iface_config_t);

    self->id          = ucs_generate_uuid((uintptr_t)self);
    self->rx_headroom = rx_headroom;
    self->data_length = self_config->super.max_bcopy;

    /* create a memory pool for data transferred */
    status = ucs_mpool_init(&self->msg_desc_mp, 0,
                            sizeof(uct_am_recv_desc_t) + rx_headroom + self->data_length,
                            sizeof(uct_am_recv_desc_t) + rx_headroom,
                            UCS_SYS_CACHE_LINE_SIZE, 16, 256, &ops, "self_msg_desc");
    if (UCS_OK != status) {
        ucs_error("Failed to create a memory pool for the loop-back transport");
        goto err;
    }

    /* set the message descriptor for the loop-back */
    self->msg_cur_desc = ucs_mpool_get(&self->msg_desc_mp);
    VALGRIND_MAKE_MEM_DEFINED(self->msg_cur_desc, sizeof(*(self->msg_cur_desc)));
    if (NULL == self->msg_cur_desc) {
        ucs_error("Failed to get the first descriptor in loop-back MP storage");
        status = UCS_ERR_NO_RESOURCE;
        goto destroy_mpool;
    }

    ucs_debug("Created a loop-back iface. id=0x%lx, desc=%p, len=%u, tx_hdr=%lu",
              self->id, self->msg_cur_desc, self->data_length, self->rx_headroom);
    return UCS_OK;

destroy_mpool:
    ucs_mpool_cleanup(&self->msg_desc_mp, 1);
err:
    return status;
}

static UCS_CLASS_CLEANUP_FUNC(uct_self_iface_t)
{
    ucs_trace_func("self=%p", self);

    if (self->msg_cur_desc) {
        ucs_mpool_put(self->msg_cur_desc);
    }
    ucs_mpool_cleanup(&self->msg_desc_mp, 1);
}

UCS_CLASS_DEFINE(uct_self_iface_t, uct_base_iface_t);
static UCS_CLASS_DEFINE_NEW_FUNC(uct_self_iface_t, uct_iface_t, uct_md_h,
                                 uct_worker_h, const char *, size_t,
                                 const uct_iface_config_t *);

static ucs_status_t uct_self_query_tl_resources(uct_md_h md,
                                                uct_tl_resource_desc_t **resource_p,
                                                unsigned *num_resources_p)
{
    uct_tl_resource_desc_t *resource = 0;

    ucs_trace_func("md=%p", md);
    resource = ucs_calloc(1, sizeof(*resource), "resource desc");
    if (NULL == resource) {
        ucs_error("Failed to allocate memory");
        return UCS_ERR_NO_MEMORY;
    }

    ucs_snprintf_zero(resource->tl_name, sizeof(resource->tl_name), "%s",
                      UCT_SELF_NAME);
    ucs_snprintf_zero(resource->dev_name, sizeof(resource->dev_name), "%s",
                      UCT_SELF_NAME);
    resource->dev_type = UCT_DEVICE_TYPE_SELF;

    *num_resources_p = 1;
    *resource_p      = resource;
    return UCS_OK;
}

UCT_TL_COMPONENT_DEFINE(uct_self_tl, uct_self_query_tl_resources, uct_self_iface_t,
                        UCT_SELF_NAME, "SELF_", uct_self_iface_config_table, uct_self_iface_config_t);
UCT_MD_REGISTER_TL(&uct_self_md, &uct_self_tl);
