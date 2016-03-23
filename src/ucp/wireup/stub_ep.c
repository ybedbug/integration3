/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "stub_ep.h"
#include "wireup.h"

#include <ucp/core/ucp_worker.h>
#include <ucs/arch/atomic.h>
#include <ucs/datastruct/queue.h>
#include <ucs/type/class.h>


/**
 * Endpoint wire-up state
 */
enum {
    UCP_STUB_EP_LOCAL_CONNECTED  = UCS_BIT(0), /* next_ep connected to remote */
    UCP_STUB_EP_REMOTE_CONNECTED = UCS_BIT(1), /* remote also connected to our next_ep */
    UCP_STUB_EP_CONNECTED        = UCP_STUB_EP_LOCAL_CONNECTED |
                                   UCP_STUB_EP_REMOTE_CONNECTED
};


/**
 * Stub endpoint, to hold off send requests until wireup process completes.
 * It is placed instead UCT endpoint before it's fully connected, and for AM
 * endpoint it also contains an auxiliary endpoint which can send wireup messages.
 */
typedef struct ucp_stub_ep {
    uct_ep_t            super;         /* Derive from uct_ep */
    ucp_ep_h            ep;            /* Pointer to the ucp_ep we're wiring */
    ucs_queue_head_t    pending_q;     /* Queue of pending operations */
    uct_ep_h            aux_ep;        /* Used to wireup the "real" endpoint */
    uct_ep_h            next_ep;       /* Next transport being wired up */

    ucp_ep_op_t         optype;        /* Which operation type inside the ucp_ep */
    ucp_rsc_index_t     aux_rsc_index; /* Index of auxiliary transport */
    volatile uint32_t   pending_count; /* Number of pending wireup operations */
    volatile uint32_t   state;         /* Endpoint state */
} ucp_stub_ep_t;
UCS_CLASS_DECLARE(ucp_stub_ep_t, ucp_ep_h, ucp_ep_op_t, unsigned,
                  const ucp_address_entry_t*);


static UCS_CLASS_DEFINE_DELETE_FUNC(ucp_stub_ep_t, uct_ep_t);

static ucs_status_t ucp_stub_ep_get_address(uct_ep_h uct_ep, uct_ep_addr_t *addr)
{
    ucp_stub_ep_t *stub_ep = ucs_derived_of(uct_ep, ucp_stub_ep_t);
    return uct_ep_get_address(stub_ep->next_ep, addr);
}

static ucs_status_t ucp_stub_ep_connect_to_ep(uct_ep_h uct_ep,
                                              const uct_device_addr_t *dev_addr,
                                              const uct_ep_addr_t *ep_addr)
{
    ucp_stub_ep_t *stub_ep = ucs_derived_of(uct_ep, ucp_stub_ep_t);
    ucs_status_t status;

    status = uct_ep_connect_to_ep(stub_ep->next_ep, dev_addr, ep_addr);
    if (status == UCS_OK) {
        stub_ep->state |= UCP_STUB_EP_LOCAL_CONNECTED;
    }
    return status;
}

static void ucp_stub_ep_progress(void *arg)
{
    ucp_stub_ep_t *stub_ep = arg;
    ucp_ep_h ep = stub_ep->ep;
    ucs_queue_head_t tmp_pending_queue;
    ucp_worker_h worker = ep->worker;
    uct_pending_req_t *req;
    ucs_status_t status;
    uct_ep_h uct_ep;
    ucp_ep_op_t optype;

    /*
     * We switch the endpoint in this function (instead in wireup code) since
     * this is guaranteed to run from the main thread.
     * Don't start using the transport before the wireup protocol finished
     * sending ack/reply.
     */
    sched_yield();
    ucs_async_check_miss(&ep->worker->async);

    /*
     * Check that we are ready to switch:
     * - Remote side must also be connected.
     * - We should have sent a wireup reply to remote side
     * - We should have sent all pending wireup operations (so we won't discard them)
     */
    if (!ucs_test_all_flags(stub_ep->state, UCP_STUB_EP_CONNECTED) ||
        (stub_ep->pending_count != 0))
    {
        return;
    }

    ucs_memory_cpu_fence();
    UCS_ASYNC_BLOCK(&worker->async);

    /* Take out next_ep */
    uct_ep = stub_ep->next_ep;

    /* Move stub pending queue to temporary queue and remove references to
     * the stub progress function
     */
    ucs_queue_head_init(&tmp_pending_queue);
    ucs_queue_for_each_extract(req, &stub_ep->pending_q, priv, 1) {
        uct_worker_progress_unregister(ep->worker->uct, ucp_stub_ep_progress,
                                       stub_ep);
        ucs_queue_push(&tmp_pending_queue, ucp_stub_ep_req_priv(req));
    }

    /* Switch to real transport */
    ep->uct_eps[stub_ep->optype] = uct_ep;
    for (optype = 0; optype < UCP_EP_OP_LAST; ++optype) {
        if (ucp_ep_config(ep)->dups[optype] == stub_ep->optype) {
            ep->uct_eps[optype] = uct_ep;
        }
    }

    /* Destroy temporary endpoints */
    uct_ep_destroy(stub_ep->aux_ep);
    uct_ep_destroy(&stub_ep->super);
    stub_ep = NULL;

    UCS_ASYNC_UNBLOCK(&worker->async);

    /* Replay pending requests */
    ucs_queue_for_each_extract(req, &tmp_pending_queue, priv, 1) {
        do {
            status = ucp_ep_add_pending_uct(ep, uct_ep, req);
        } while (status != UCS_OK);
        --ep->worker->stub_pend_count;
    }
}

static ucs_status_t ucp_stub_ep_send_func(uct_ep_h uct_ep)
{
    ucp_stub_ep_t *stub_ep = ucs_derived_of(uct_ep, ucp_stub_ep_t);
    ucp_stub_ep_progress(stub_ep);
    return UCS_ERR_NO_RESOURCE;
}

static ssize_t ucp_stub_ep_bcopy_send_func(uct_ep_h uct_ep)
{
    ucp_stub_ep_t *stub_ep = ucs_derived_of(uct_ep, ucp_stub_ep_t);
    ucp_stub_ep_progress(stub_ep);
    return UCS_ERR_NO_RESOURCE;
}

static uct_ep_h ucp_stub_ep_get_wireup_msg_ep(ucp_stub_ep_t *stub_ep)
{
    uct_ep_h wireup_msg_ep;

    if (ucs_test_all_flags(stub_ep->state, UCP_STUB_EP_CONNECTED)) {
        wireup_msg_ep = stub_ep->next_ep;
    } else {
        wireup_msg_ep = stub_ep->aux_ep;
    }
    ucs_assert(wireup_msg_ep != NULL);
    return wireup_msg_ep;
}

static ucs_status_t ucp_stub_pending_add(uct_ep_h uct_ep, uct_pending_req_t *req)
{
    ucp_stub_ep_t *stub_ep = ucs_derived_of(uct_ep, ucp_stub_ep_t);
    ucp_ep_h ep;

    if (req->func == ucp_wireup_msg_progress) {
        return uct_ep_pending_add(ucp_stub_ep_get_wireup_msg_ep(stub_ep), req);
    }

    ep = stub_ep->ep;
    ucs_queue_push(&stub_ep->pending_q, ucp_stub_ep_req_priv(req));
    ucs_atomic_add32(&stub_ep->pending_count, 1);
    ++ep->worker->stub_pend_count;

    /* Add a reference to the dummy progress function. If we have a pending
     * request and this endpoint is still doing wireup, we must make sure progress
     * is made.
     * TODO one progress per worker
     * */
    uct_worker_progress_register(ep->worker->uct, ucp_stub_ep_progress, stub_ep);
    return UCS_OK;
}

static ssize_t ucp_stub_ep_am_bcopy(uct_ep_h uct_ep, uint8_t id,
                                    uct_pack_callback_t pack_cb, void *arg)
{
    ucp_stub_ep_t *stub_ep = ucs_derived_of(uct_ep, ucp_stub_ep_t);

    if (id == UCP_AM_ID_WIREUP) {
        return uct_ep_am_bcopy(ucp_stub_ep_get_wireup_msg_ep(stub_ep),
                               UCP_AM_ID_WIREUP, pack_cb, arg);

    }

    ucp_stub_ep_progress(stub_ep);
    return UCS_ERR_NO_RESOURCE;
}

static void ucp_stub_pending_purge(uct_ep_h uct_ep, uct_pending_callback_t cb)
{
    ucp_stub_ep_t *stub_ep = ucs_derived_of(uct_ep, ucp_stub_ep_t);
    ucs_assert_always(ucs_queue_is_empty(&stub_ep->pending_q));
}

static uct_iface_t ucp_stub_iface = {
    .ops = {
        .ep_get_address       = ucp_stub_ep_get_address,
        .ep_connect_to_ep     = ucp_stub_ep_connect_to_ep,
        .ep_flush             = (void*)ucs_empty_function_return_inprogress,
        .ep_destroy           = UCS_CLASS_DELETE_FUNC_NAME(ucp_stub_ep_t),
        .ep_pending_add       = ucp_stub_pending_add,
        .ep_pending_purge     = ucp_stub_pending_purge,
        .ep_put_short         = (void*)ucp_stub_ep_send_func,
        .ep_put_bcopy         = (void*)ucp_stub_ep_bcopy_send_func,
        .ep_put_zcopy         = (void*)ucp_stub_ep_send_func,
        .ep_get_bcopy         = (void*)ucp_stub_ep_send_func,
        .ep_get_zcopy         = (void*)ucp_stub_ep_send_func,
        .ep_am_short          = (void*)ucp_stub_ep_send_func,
        .ep_am_bcopy          = ucp_stub_ep_am_bcopy,
        .ep_am_zcopy          = (void*)ucp_stub_ep_send_func,
        .ep_atomic_add64      = (void*)ucp_stub_ep_send_func,
        .ep_atomic_fadd64     = (void*)ucp_stub_ep_send_func,
        .ep_atomic_swap64     = (void*)ucp_stub_ep_send_func,
        .ep_atomic_cswap64    = (void*)ucp_stub_ep_send_func,
        .ep_atomic_add32      = (void*)ucp_stub_ep_send_func,
        .ep_atomic_fadd32     = (void*)ucp_stub_ep_send_func,
        .ep_atomic_swap32     = (void*)ucp_stub_ep_send_func,
        .ep_atomic_cswap32    = (void*)ucp_stub_ep_send_func
    }
};

UCS_CLASS_DEFINE_NAMED_NEW_FUNC(ucp_stub_ep_create, ucp_stub_ep_t, uct_ep_t,
                                ucp_ep_h, ucp_ep_op_t, unsigned,
                                const ucp_address_entry_t*);

static double ucp_wireup_aux_score_func(ucp_worker_h worker,
                                        uct_iface_attr_t *iface_attr,
                                        char *reason, size_t max)
{
    if (!(iface_attr->cap.flags & UCT_IFACE_FLAG_AM_BCOPY)) {
        strncpy(reason, "am_bcopy for wireup", max);
        return 0.0;
    }

    if (!(iface_attr->cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE)) {
        strncpy(reason, "connecting to iface", max);
        return 0.0;
    }

    if (!(iface_attr->cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE)) {
        strncpy(reason, "async am callback", max);
        return 0.0;
    }

    if (!(iface_attr->cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE)) {
        strncpy(reason, "pending", max);
        return 0.0;
    }

    return (1e-3 / iface_attr->latency) +
           (1e3 * ucs_max(iface_attr->cap.am.max_bcopy, iface_attr->cap.am.max_short));
}

static ucs_status_t
ucp_stub_ep_connect_aux(ucp_stub_ep_t *stub_ep, unsigned address_count,
                        const ucp_address_entry_t *address_list)
{
    ucp_ep_h ep            = stub_ep->ep;
    ucp_worker_h worker    = ep->worker;
    const ucp_address_entry_t *aux_addr;
    unsigned aux_addr_index;
    ucs_status_t status;

    /* select an auxiliary transport which would be used to pass connection
     * establishment messages.
     */
    status = ucp_select_transport(worker, ucp_ep_peer_name(ep), address_list,
                                  address_count, UCP_NULL_RESOURCE,
                                  &stub_ep->aux_rsc_index, &aux_addr_index,
                                  ucp_wireup_aux_score_func, "auxiliary");
    if (status != UCS_OK) {
        return status;
    }

    aux_addr = &address_list[aux_addr_index];

    /* create auxiliary endpoint connected to the remote iface. */
    ucs_assert(aux_addr->tl_addr_len > 0);
    status = uct_ep_create_connected(worker->ifaces[stub_ep->aux_rsc_index],
                                     aux_addr->dev_addr, aux_addr->iface_addr,
                                     &stub_ep->aux_ep);
    if (status != UCS_OK) {
        return status;
    }

    return UCS_OK;
}

UCS_CLASS_INIT_FUNC(ucp_stub_ep_t, ucp_ep_h ep, ucp_ep_op_t optype,
                    unsigned address_count, const ucp_address_entry_t *address_list)
{
    ucp_worker_h worker = ep->worker;
    ucp_rsc_index_t rsc_index;
    ucs_status_t status;

    self->super.iface   = &ucp_stub_iface;
    self->ep            = ep;
    self->aux_ep        = NULL;
    self->optype        = optype;
    self->aux_rsc_index = UCP_NULL_RESOURCE;
    self->pending_count = 0;
    self->state         = 0;
    ucs_queue_head_init(&self->pending_q);

    /* create endpoint for the real transport, which we would eventually connect */
    if (ep->cfg_index != (uint8_t)-1) {
        rsc_index = ucp_ep_config(ep)->rscs[optype];
        status = uct_ep_create(worker->ifaces[rsc_index], &self->next_ep);
        if (status != UCS_OK) {
            return status;
        }

        /* we need to create an auxiliary transport only for active messages */
        if (optype == UCP_EP_OP_AM) {
            status = ucp_stub_ep_connect_aux(self, address_count, address_list);
            if (status != UCS_OK) {
                uct_ep_destroy(self->next_ep);
                return status;
            }
        }
    }

    // TODO print also aux_ep,next_ep if exist
    ucs_debug("created stub ep to %s for %s "
//              "[next_ep %p " UCT_TL_RESOURCE_DESC_FMT "] ",
              , ucp_ep_peer_name(ep), ucp_wireup_ep_ops[optype].title
//            ,  self->next_ep, UCT_TL_RESOURCE_DESC_ARG(&worker->context->tl_rscs[rsc_index].tl_rsc)
              );
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(ucp_stub_ep_t)
{
    ucs_assert(ucs_queue_is_empty(&self->pending_q));
    if (self->aux_ep != NULL) {
        uct_ep_destroy(self->aux_ep);
    }
    uct_ep_destroy(self->next_ep);
}

UCS_CLASS_DEFINE(ucp_stub_ep_t, void);

ucp_rsc_index_t ucp_stub_ep_get_aux_rsc_index(uct_ep_h uct_ep)
{
    ucp_stub_ep_t *stub_ep = ucs_derived_of(uct_ep, ucp_stub_ep_t);

    if (uct_ep->iface != &ucp_stub_iface) {
        return UCP_NULL_RESOURCE;
    }

    ucs_assert(stub_ep->aux_ep != NULL);
    return stub_ep->aux_rsc_index;
}

void ucp_stub_ep_remote_connected(uct_ep_h uct_ep)
{
    ucp_stub_ep_t *stub_ep = ucs_derived_of(uct_ep, ucp_stub_ep_t);

    ucs_assert(uct_ep->iface != &ucp_stub_iface);
    stub_ep->state |= UCP_STUB_EP_REMOTE_CONNECTED;
}
