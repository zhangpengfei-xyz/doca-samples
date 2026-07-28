#include "srdma_admin.h"

#include "../common/vfio_adminq_abi.h"

#include <string.h>

enum {
    SRDMA_QP_TYPE_GSI = 1,
    SRDMA_QP_TYPE_RC = 2,
    SRDMA_QP_STATE_RESET = 0,
    SRDMA_QP_STATE_INIT = 1,
    SRDMA_QP_STATE_RTR = 2,
    SRDMA_QP_STATE_RTS = 3,
    SRDMA_QP_STATE_ERR = 6,
};

static bool aligned(uint64_t value, uint64_t alignment)
{
    return value != 0 && (value & (alignment - 1)) == 0;
}

static unsigned int alloc_resource(struct srdma_admin_resource *table,
                                   unsigned int count, size_t stride)
{
    for (unsigned int i = 1; i <= count; i++) {
        struct srdma_admin_resource *resource =
            (void *)((uint8_t *)table + i * stride);
        if (!resource->allocated) {
            resource->allocated = true;
            resource->generation++;
            resource->refs = 0;
            return i;
        }
    }
    return 0;
}

void srdma_admin_init(struct srdma_admin *admin, uint32_t generation)
{
    memset(admin, 0, sizeof(*admin));
    admin->generation = generation;
    admin->uctx[0].allocated = true;
    admin->uctx[0].generation = generation;
}

void srdma_admin_reset(struct srdma_admin *admin, uint32_t generation)
{
    srdma_admin_init(admin, generation);
}

static enum srdma_admin_rc get_cap(struct srdma_admin_entry *out)
{
    /* 4 KiB pages, 128 GIDs, CQ PAL, conservative control-plane limits. */
    srdma_admin_set(out, 2, 0, 32, 0);
    srdma_admin_set(out, 3, 0, 8, 0x11);
    srdma_admin_set(out, 3, 8, 20, 1);
    srdma_admin_set(out, 3, 28, 4, 7);
    srdma_admin_set(out, 4, 0, 32, 1U << 8);
    srdma_admin_set(out, 6, 0, 4, 6);
    srdma_admin_set(out, 6, 8, 4, 4);
    srdma_admin_set(out, 6, 16, 4, 6);
    srdma_admin_set(out, 6, 20, 6, 33);
    srdma_admin_set(out, 6, 26, 6, 31);
    srdma_admin_set(out, 7, 0, 5, 4);
    srdma_admin_set(out, 7, 5, 5, 4);
    srdma_admin_set(out, 7, 24, 8, 32);
    srdma_admin_set(out, 8, 0, 5, 13);
    srdma_admin_set(out, 8, 5, 5, 15);
    srdma_admin_set(out, 8, 15, 5, 19);
    srdma_admin_set(out, 8, 20, 5, 12);
    srdma_admin_set(out, 8, 25, 4, 12);
    srdma_admin_set(out, 8, 29, 3, 2);
    srdma_admin_set(out, 10, 12, 20, 1);
    srdma_admin_set(out, 11, 12, 20, 1);
    srdma_admin_set(out, 22, 0, 32, 1);
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc create_eq(struct srdma_admin *admin,
                                     const struct srdma_admin_entry *in,
                                     struct srdma_admin_entry *out)
{
    uint8_t log_depth = srdma_admin_get(in, 0, 0, 5);
    uint16_t vector = srdma_admin_get(in, 0, 16, 16);
    uint64_t addr = ((uint64_t)in->dw[2] << 32) |
                    ((uint64_t)srdma_admin_get(in, 1, 12, 20) << 12);
    unsigned int handle;

    if (log_depth == 0 || log_depth > 12 || vector == 0 ||
        vector >= VFIO_ADMINQ_NUM_MSIX || !aligned(addr, 4096))
        return SRDMA_ADMIN_RC_INVALID_ARG;
    handle = alloc_resource(&admin->eq[0].resource, SRDMA_ADMIN_MAX_EQ,
                            sizeof(admin->eq[0]));
    if (handle == 0)
        return SRDMA_ADMIN_RC_NO_RESOURCE;
    admin->eq[handle].addr = addr;
    admin->eq[handle].vector = vector;
    admin->eq[handle].log_depth = log_depth;
    srdma_admin_set(out, 0, 0, 16, handle);
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc destroy_eq(struct srdma_admin *admin,
                                      const struct srdma_admin_entry *in)
{
    uint16_t handle = srdma_admin_get(in, 0, 0, 16);
    if (handle == 0 || handle > SRDMA_ADMIN_MAX_EQ ||
        !admin->eq[handle].resource.allocated)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    if (admin->eq[handle].resource.refs != 0)
        return SRDMA_ADMIN_RC_RESOURCE_BUSY;
    admin->eq[handle].resource.allocated = false;
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc alloc_uctx(struct srdma_admin *admin,
                                     const struct srdma_admin_entry *in)
{
    uint16_t id = srdma_admin_get(in, 0, 0, 16);
    if (id == 0 || id >= SRDMA_ADMIN_MAX_UCTX)
        return id >= SRDMA_ADMIN_MAX_UCTX ? SRDMA_ADMIN_RC_NO_RESOURCE :
                                           SRDMA_ADMIN_RC_INVALID_ARG;
    if (admin->uctx[id].allocated)
        return SRDMA_ADMIN_RC_RESOURCE_BUSY;
    admin->uctx[id].allocated = true;
    admin->uctx[id].generation++;
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc dealloc_uctx(struct srdma_admin *admin,
                                       const struct srdma_admin_entry *in)
{
    uint16_t id = srdma_admin_get(in, 0, 0, 16);
    if (id == 0 || id >= SRDMA_ADMIN_MAX_UCTX || !admin->uctx[id].allocated)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    if (admin->uctx[id].refs != 0)
        return SRDMA_ADMIN_RC_RESOURCE_BUSY;
    admin->uctx[id].allocated = false;
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc alloc_pd(struct srdma_admin *admin,
                                   const struct srdma_admin_entry *in,
                                   struct srdma_admin_entry *out)
{
    uint16_t uctx = srdma_admin_get(in, 0, 0, 16);
    unsigned int handle;
    if (uctx >= SRDMA_ADMIN_MAX_UCTX || !admin->uctx[uctx].allocated)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    handle = alloc_resource(&admin->pd[0].resource, SRDMA_ADMIN_MAX_PD,
                            sizeof(admin->pd[0]));
    if (handle == 0)
        return SRDMA_ADMIN_RC_NO_RESOURCE;
    admin->pd[handle].uctx = uctx;
    admin->uctx[uctx].refs++;
    srdma_admin_set(out, 0, 0, 24, handle);
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc dealloc_pd(struct srdma_admin *admin,
                                     const struct srdma_admin_entry *in)
{
    uint32_t handle = srdma_admin_get(in, 0, 0, 24);
    if (handle == 0 || handle > SRDMA_ADMIN_MAX_PD ||
        !admin->pd[handle].resource.allocated)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    if (admin->pd[handle].resource.refs != 0)
        return SRDMA_ADMIN_RC_RESOURCE_BUSY;
    admin->uctx[admin->pd[handle].uctx].refs--;
    admin->pd[handle].resource.allocated = false;
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc reg_mr(struct srdma_admin *admin,
                                 const struct srdma_admin_entry *in,
                                 struct srdma_admin_entry *out)
{
    uint32_t pd = srdma_admin_get(in, 0, 0, 24);
    uint8_t log_page = srdma_admin_get(in, 1, 0, 6);
    uint8_t level = srdma_admin_get(in, 1, 12, 2);
    uint8_t type = srdma_admin_get(in, 1, 14, 3);
    uint64_t length = srdma_admin_get64(in, 4);
    uint64_t pa0 = srdma_admin_get64(in, 6);
    uint64_t page_size;
    uint64_t page_offset = in->dw[14];
    uint64_t pages;
    unsigned int handle;
    uint32_t mkey;

    if (pd == 0 || pd > SRDMA_ADMIN_MAX_PD ||
        !admin->pd[pd].resource.allocated)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    if (type != 0 || (level != 0 && level != 2) || log_page < 12 ||
        log_page > 31 || length == 0 || !aligned(pa0, 4096))
        return SRDMA_ADMIN_RC_INVALID_ARG;
    page_size = UINT64_C(1) << log_page;
    if (page_offset >= page_size ||
        length > UINT64_MAX - page_offset - (page_size - 1))
        return SRDMA_ADMIN_RC_INVALID_ARG;
    pages = level == 0 ?
        (page_offset + length + page_size - 1) / page_size : in->dw[9];
    if (pages == 0 ||
        (level == 0 && pages > 4))
        return SRDMA_ADMIN_RC_INVALID_ARG;
    handle = alloc_resource(&admin->mr[0].resource, SRDMA_ADMIN_MAX_MR,
                            sizeof(admin->mr[0]));
    if (handle == 0)
        return SRDMA_ADMIN_RC_NO_RESOURCE;
    mkey = (admin->mr[handle].resource.generation << 16) | handle;
    admin->mr[handle].pd = pd;
    admin->mr[handle].va = srdma_admin_get64(in, 2);
    admin->mr[handle].length = length;
    admin->mr[handle].mkey = mkey;
    admin->pd[pd].resource.refs++;
    srdma_admin_set(out, 0, 0, 32, mkey);
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc dereg_mr(struct srdma_admin *admin,
                                   const struct srdma_admin_entry *in)
{
    uint32_t mkey = in->dw[0];
    uint32_t handle = mkey & 0xffffU;
    if (handle == 0 || handle > SRDMA_ADMIN_MAX_MR ||
        !admin->mr[handle].resource.allocated ||
        admin->mr[handle].mkey != mkey)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    admin->pd[admin->mr[handle].pd].resource.refs--;
    admin->mr[handle].resource.allocated = false;
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc create_cq(struct srdma_admin *admin,
                                     const struct srdma_admin_entry *in,
                                     struct srdma_admin_entry *out)
{
    uint16_t uctx = srdma_admin_get(in, 0, 0, 16);
    uint8_t log_depth = srdma_admin_get(in, 0, 24, 5);
    uint16_t eq = srdma_admin_get(in, 1, 0, 16);
    uint8_t cont = srdma_admin_get(in, 1, 31, 1);
    uint64_t dbr = srdma_admin_get64(in, 2);
    uint64_t pa0 = srdma_admin_get64(in, 4);
    unsigned int handle;
    if (uctx >= SRDMA_ADMIN_MAX_UCTX || !admin->uctx[uctx].allocated ||
        eq == 0 || eq > SRDMA_ADMIN_MAX_EQ ||
        !admin->eq[eq].resource.allocated)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    if (log_depth == 0 || log_depth > 19 || !aligned(dbr, 64) ||
        !aligned(pa0, 4096) || (!cont && srdma_admin_get(in, 7, 0, 32) == 0))
        return SRDMA_ADMIN_RC_INVALID_ARG;
    handle = alloc_resource(&admin->cq[0].resource, SRDMA_ADMIN_MAX_CQ,
                            sizeof(admin->cq[0]));
    if (handle == 0)
        return SRDMA_ADMIN_RC_NO_RESOURCE;
    admin->cq[handle].uctx = uctx;
    admin->cq[handle].eq = eq;
    admin->cq[handle].log_depth = log_depth;
    admin->uctx[uctx].refs++;
    admin->eq[eq].resource.refs++;
    srdma_admin_set(out, 0, 0, 24, handle);
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc destroy_cq(struct srdma_admin *admin,
                                      const struct srdma_admin_entry *in)
{
    uint32_t handle = srdma_admin_get(in, 0, 0, 24);
    if (handle == 0 || handle > SRDMA_ADMIN_MAX_CQ ||
        !admin->cq[handle].resource.allocated)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    if (admin->cq[handle].resource.refs != 0)
        return SRDMA_ADMIN_RC_RESOURCE_BUSY;
    admin->uctx[admin->cq[handle].uctx].refs--;
    admin->eq[admin->cq[handle].eq].resource.refs--;
    admin->cq[handle].resource.allocated = false;
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc create_qp(struct srdma_admin *admin,
                                     const struct srdma_admin_entry *in,
                                     struct srdma_admin_entry *out)
{
    uint32_t pd = srdma_admin_get(in, 0, 0, 24);
    uint8_t type = srdma_admin_get(in, 0, 24, 8);
    uint32_t scq = srdma_admin_get(in, 1, 0, 24);
    uint32_t rcq = srdma_admin_get(in, 2, 0, 24);
    bool sq_cont = srdma_admin_get(in, 1, 31, 1);
    bool rq_cont = srdma_admin_get(in, 2, 31, 1);
    uint64_t sq_pa = ((uint64_t)in->dw[9] << 32) |
                     (in->dw[8] & UINT32_C(0xfffff000));
    uint64_t rq_pa = ((uint64_t)in->dw[11] << 32) |
                     (in->dw[10] & UINT32_C(0xfffff000));
    unsigned int handle;
    if (pd == 0 || pd > SRDMA_ADMIN_MAX_PD ||
        !admin->pd[pd].resource.allocated || scq == 0 ||
        scq > SRDMA_ADMIN_MAX_CQ || !admin->cq[scq].resource.allocated ||
        rcq == 0 || rcq > SRDMA_ADMIN_MAX_CQ ||
        !admin->cq[rcq].resource.allocated)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    if ((type != SRDMA_QP_TYPE_RC && type != SRDMA_QP_TYPE_GSI) ||
        !sq_cont || !rq_cont || !aligned(srdma_admin_get64(in, 6), 64) ||
        !aligned(sq_pa, 4096) || !aligned(rq_pa, 4096))
        return SRDMA_ADMIN_RC_INVALID_ARG;
    handle = alloc_resource(&admin->qp[0].resource, SRDMA_ADMIN_MAX_QP,
                            sizeof(admin->qp[0]));
    if (handle == 0)
        return SRDMA_ADMIN_RC_NO_RESOURCE;
    admin->qp[handle].pd = pd;
    admin->qp[handle].scq = scq;
    admin->qp[handle].rcq = rcq;
    admin->qp[handle].type = type;
    admin->qp[handle].state = SRDMA_QP_STATE_RESET;
    admin->pd[pd].resource.refs++;
    admin->cq[scq].resource.refs++;
    admin->cq[rcq].resource.refs++;
    srdma_admin_set(out, 0, 0, 24, handle);
    return SRDMA_ADMIN_RC_SUCC;
}

static struct srdma_admin_qp *get_qp(struct srdma_admin *admin,
                                    const struct srdma_admin_entry *in)
{
    uint32_t qpn = srdma_admin_get(in, 0, 0, 24);
    if (qpn == 0 || qpn > SRDMA_ADMIN_MAX_QP ||
        !admin->qp[qpn].resource.allocated)
        return NULL;
    return &admin->qp[qpn];
}

static enum srdma_admin_rc destroy_qp(struct srdma_admin *admin,
                                      const struct srdma_admin_entry *in)
{
    struct srdma_admin_qp *qp = get_qp(admin, in);
    if (qp == NULL)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    admin->pd[qp->pd].resource.refs--;
    admin->cq[qp->scq].resource.refs--;
    admin->cq[qp->rcq].resource.refs--;
    qp->resource.allocated = false;
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc modify_qp(struct srdma_admin *admin, uint8_t opcode,
                                     const struct srdma_admin_entry *in)
{
    struct srdma_admin_qp *qp = get_qp(admin, in);
    if (qp == NULL)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    switch (opcode) {
    case SRDMA_ADMIN_OP_RST2INIT_QP:
        if (qp->state != SRDMA_QP_STATE_RESET)
            return SRDMA_ADMIN_RC_INVALID_ARG;
        qp->state = SRDMA_QP_STATE_INIT;
        qp->port = srdma_admin_get(in, 0, 24, 8);
        qp->qkey = in->dw[1];
        qp->access = srdma_admin_get(in, 2, 0, 4);
        break;
    case SRDMA_ADMIN_OP_INIT2INIT_QP:
        if (qp->state != SRDMA_QP_STATE_INIT)
            return SRDMA_ADMIN_RC_INVALID_ARG;
        if (srdma_admin_get(in, 0, 24, 1))
            qp->qkey = in->dw[1];
        if (srdma_admin_get(in, 0, 25, 1))
            qp->access = srdma_admin_get(in, 2, 0, 4);
        if (srdma_admin_get(in, 0, 26, 1))
            qp->port = srdma_admin_get(in, 3, 0, 8);
        break;
    case SRDMA_ADMIN_OP_INIT2RTR_QP:
        if (qp->state != SRDMA_QP_STATE_INIT)
            return SRDMA_ADMIN_RC_INVALID_ARG;
        qp->dest_qpn = srdma_admin_get(in, 1, 0, 24);
        qp->rq_psn = srdma_admin_get(in, 2, 0, 24);
        qp->min_rnr = srdma_admin_get(in, 1, 24, 5);
        qp->ra_res = srdma_admin_get(in, 2, 24, 3);
        qp->log_mtu = srdma_admin_get(in, 2, 28, 4);
        if (qp->log_mtu > 12)
            return SRDMA_ADMIN_RC_INVALID_ARG;
        qp->sgid_idx = srdma_admin_get(in, 3, 0, 8);
        qp->hop_limit = srdma_admin_get(in, 3, 8, 8);
        qp->traffic_class = srdma_admin_get(in, 3, 16, 8);
        qp->static_rate = srdma_admin_get(in, 3, 24, 8);
        qp->flow_label = srdma_admin_get(in, 4, 0, 20);
        qp->dscp = srdma_admin_get(in, 4, 20, 6);
        qp->eth_prio = srdma_admin_get(in, 4, 26, 3);
        memcpy(qp->dgid, &in->dw[5], sizeof(qp->dgid));
        memcpy(qp->dmac, &in->dw[9], sizeof(qp->dmac));
        qp->udp_sport = srdma_admin_get(in, 10, 16, 16);
        if (srdma_admin_get(in, 0, 24, 1))
            qp->qkey = in->dw[11];
        if (srdma_admin_get(in, 0, 25, 1))
            qp->access = srdma_admin_get(in, 12, 0, 4);
        qp->state = SRDMA_QP_STATE_RTR;
        break;
    case SRDMA_ADMIN_OP_RTR2RTS_QP:
        if (qp->state != SRDMA_QP_STATE_RTR)
            return SRDMA_ADMIN_RC_INVALID_ARG;
        qp->sq_psn = srdma_admin_get(in, 1, 0, 24);
        qp->timeout = srdma_admin_get(in, 1, 24, 5);
        qp->retry_cnt = srdma_admin_get(in, 1, 29, 3);
        qp->rnr_retry = srdma_admin_get(in, 2, 0, 3);
        qp->ra_req = srdma_admin_get(in, 2, 3, 3);
        if (srdma_admin_get(in, 0, 24, 1))
            qp->qkey = in->dw[3];
        if (srdma_admin_get(in, 0, 25, 1))
            qp->access = srdma_admin_get(in, 4, 0, 4);
        if (srdma_admin_get(in, 0, 26, 1))
            qp->min_rnr = srdma_admin_get(in, 5, 0, 5);
        qp->state = SRDMA_QP_STATE_RTS;
        break;
    case SRDMA_ADMIN_OP_RTS2RTS_QP:
        if (qp->state != SRDMA_QP_STATE_RTS)
            return SRDMA_ADMIN_RC_INVALID_ARG;
        if (srdma_admin_get(in, 0, 24, 1))
            qp->qkey = in->dw[1];
        if (srdma_admin_get(in, 0, 25, 1))
            qp->access = srdma_admin_get(in, 2, 0, 4);
        if (srdma_admin_get(in, 0, 26, 1))
            qp->min_rnr = srdma_admin_get(in, 3, 0, 5);
        break;
    case SRDMA_ADMIN_OP_2ERR_QP:
        qp->state = SRDMA_QP_STATE_ERR;
        break;
    case SRDMA_ADMIN_OP_2RST_QP:
        qp->state = SRDMA_QP_STATE_RESET;
        qp->dest_qpn = qp->sq_psn = qp->rq_psn = 0;
        break;
    default:
        return SRDMA_ADMIN_RC_OP_NOT_SUPP;
    }
    return SRDMA_ADMIN_RC_SUCC;
}

static enum srdma_admin_rc query_qp(struct srdma_admin *admin,
                                    const struct srdma_admin_entry *in,
                                    struct srdma_admin_entry *out)
{
    struct srdma_admin_qp *qp = get_qp(admin, in);
    if (qp == NULL)
        return SRDMA_ADMIN_RC_INVALID_IDX;
    srdma_admin_set(out, 0, 0, 3, qp->state);
    srdma_admin_set(out, 0, 4, 4, qp->log_mtu);
    srdma_admin_set(out, 0, 8, 8, qp->type);
    srdma_admin_set(out, 1, 0, 24, qp->pd);
    srdma_admin_set(out, 1, 24, 8, qp->port);
    srdma_admin_set(out, 2, 0, 24, qp->scq);
    srdma_admin_set(out, 3, 0, 24, qp->rcq);
    out->dw[5] = qp->qkey;
    srdma_admin_set(out, 6, 0, 24, qp->dest_qpn);
    srdma_admin_set(out, 6, 24, 5, qp->min_rnr);
    srdma_admin_set(out, 6, 29, 3, qp->rnr_retry);
    srdma_admin_set(out, 7, 0, 24, qp->sq_psn);
    srdma_admin_set(out, 7, 24, 5, qp->timeout);
    srdma_admin_set(out, 7, 29, 3, qp->retry_cnt);
    srdma_admin_set(out, 8, 0, 24, qp->rq_psn);
    srdma_admin_set(out, 8, 24, 3, qp->ra_res);
    srdma_admin_set(out, 8, 27, 3, qp->ra_req);
    srdma_admin_set(out, 9, 0, 4, qp->access);
    srdma_admin_set(out, 10, 0, 8, qp->sgid_idx);
    srdma_admin_set(out, 10, 8, 8, qp->hop_limit);
    srdma_admin_set(out, 10, 16, 8, qp->traffic_class);
    srdma_admin_set(out, 10, 24, 8, qp->static_rate);
    srdma_admin_set(out, 11, 0, 20, qp->flow_label);
    srdma_admin_set(out, 11, 20, 6, qp->dscp);
    srdma_admin_set(out, 11, 26, 3, qp->eth_prio);
    memcpy(&out->dw[12], qp->dgid, sizeof(qp->dgid));
    memcpy(&out->dw[16], qp->dmac, sizeof(qp->dmac));
    srdma_admin_set(out, 17, 16, 16, qp->udp_sport);
    return SRDMA_ADMIN_RC_SUCC;
}

enum srdma_admin_rc srdma_admin_execute(struct srdma_admin *admin,
                                        uint8_t opcode,
                                        const struct srdma_admin_entry *in,
                                        struct srdma_admin_entry *out)
{
    enum srdma_admin_rc rc;
    memset(out, 0, sizeof(*out));
    admin->commands++;
    switch (opcode) {
    case SRDMA_ADMIN_OP_GET_CAP:
        rc = get_cap(out); break;
    case SRDMA_ADMIN_OP_GET_CAP_EXT:
        out->dw[0] = SRDMA_ADMIN_MAX_EQ;
        out->dw[1] = SRDMA_ADMIN_MAX_PD;
        out->dw[2] = SRDMA_ADMIN_MAX_MR;
        out->dw[3] = SRDMA_ADMIN_MAX_CQ;
        out->dw[4] = SRDMA_ADMIN_MAX_QP;
        rc = SRDMA_ADMIN_RC_SUCC; break;
    case SRDMA_ADMIN_OP_HEALTH_CHECK:
    case SRDMA_ADMIN_OP_GET_STATS:
    case SRDMA_ADMIN_OP_GET_STATS_EXT:
    case SRDMA_ADMIN_OP_GET_STATS_EXT3:
        rc = SRDMA_ADMIN_RC_SUCC; break;
    case SRDMA_ADMIN_OP_GET_STATS_EXT2:
        out->dw[0] = 0x101U;
        rc = SRDMA_ADMIN_RC_SUCC; break;
    case SRDMA_ADMIN_OP_NETDEV_UP:
        admin->netdev_up = true; rc = SRDMA_ADMIN_RC_SUCC; break;
    case SRDMA_ADMIN_OP_NETDEV_DOWN:
        admin->netdev_up = false; rc = SRDMA_ADMIN_RC_SUCC; break;
    case SRDMA_ADMIN_OP_ADD_GID: {
        uint16_t id = srdma_admin_get(in, 6, 0, 16);
        if (id >= SRDMA_ADMIN_MAX_GID) rc = SRDMA_ADMIN_RC_INVALID_ARG;
        else { admin->gid[id] = true; rc = SRDMA_ADMIN_RC_SUCC; }
        break;
    }
    case SRDMA_ADMIN_OP_DEL_GID: {
        uint16_t id = srdma_admin_get(in, 0, 0, 16);
        if (id >= SRDMA_ADMIN_MAX_GID) rc = SRDMA_ADMIN_RC_INVALID_ARG;
        else { admin->gid[id] = false; rc = SRDMA_ADMIN_RC_SUCC; }
        break;
    }
    case SRDMA_ADMIN_OP_CREATE_EQ: rc = create_eq(admin, in, out); break;
    case SRDMA_ADMIN_OP_DESTROY_EQ: rc = destroy_eq(admin, in); break;
    case SRDMA_ADMIN_OP_ALLOC_UCTX: rc = alloc_uctx(admin, in); break;
    case SRDMA_ADMIN_OP_DEALLOC_UCTX: rc = dealloc_uctx(admin, in); break;
    case SRDMA_ADMIN_OP_ALLOC_PD: rc = alloc_pd(admin, in, out); break;
    case SRDMA_ADMIN_OP_DEALLOC_PD: rc = dealloc_pd(admin, in); break;
    case SRDMA_ADMIN_OP_REG_MR: rc = reg_mr(admin, in, out); break;
    case SRDMA_ADMIN_OP_DEREG_MR: rc = dereg_mr(admin, in); break;
    case SRDMA_ADMIN_OP_CREATE_CQ: rc = create_cq(admin, in, out); break;
    case SRDMA_ADMIN_OP_DESTROY_CQ: rc = destroy_cq(admin, in); break;
    case SRDMA_ADMIN_OP_CREATE_QP: rc = create_qp(admin, in, out); break;
    case SRDMA_ADMIN_OP_DESTROY_QP: rc = destroy_qp(admin, in); break;
    case SRDMA_ADMIN_OP_QUERY_QP: rc = query_qp(admin, in, out); break;
    case SRDMA_ADMIN_OP_RST2INIT_QP:
    case SRDMA_ADMIN_OP_INIT2INIT_QP:
    case SRDMA_ADMIN_OP_INIT2RTR_QP:
    case SRDMA_ADMIN_OP_RTR2RTS_QP:
    case SRDMA_ADMIN_OP_RTS2RTS_QP:
    case SRDMA_ADMIN_OP_2ERR_QP:
    case SRDMA_ADMIN_OP_2RST_QP:
        rc = modify_qp(admin, opcode, in); break;
    case SRDMA_ADMIN_OP_ENABLE_MS_MONITOR:
    case SRDMA_ADMIN_OP_DISABLE_MS_MONITOR:
        rc = SRDMA_ADMIN_RC_OP_NOT_SUPP; break;
    default:
        rc = SRDMA_ADMIN_RC_OP_NOT_SUPP; break;
    }
    if (rc != SRDMA_ADMIN_RC_SUCC)
        admin->failures++;
    return rc;
}
