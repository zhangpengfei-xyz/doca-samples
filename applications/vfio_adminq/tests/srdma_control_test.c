#include "../common/vfio_adminq_abi.h"
#include "../dev-be/srdma_admin.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static enum srdma_admin_rc execute(struct srdma_admin *admin, uint8_t opcode,
                                   struct srdma_admin_entry *in,
                                   struct srdma_admin_entry *out)
{
    return srdma_admin_execute(admin, opcode, in, out);
}

static int test_admin_resources(void)
{
    struct srdma_admin_entry in = {0};
    struct srdma_admin_entry out;
    struct srdma_admin admin;
    uint32_t pd;
    uint32_t eq;
    uint32_t cq;
    uint32_t qp;
    uint32_t mkey;

    srdma_admin_init(&admin, 7);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_GET_CAP, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);

    srdma_admin_set(&in, 0, 0, 16, 1);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_ALLOC_UCTX, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    srdma_admin_set(&in, 0, 0, 16, SRDMA_ADMIN_MAX_UCTX);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_ALLOC_UCTX, &in, &out) ==
          SRDMA_ADMIN_RC_NO_RESOURCE);

    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 16, 1);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_ALLOC_PD, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    pd = srdma_admin_get(&out, 0, 0, 24);
    CHECK(pd != 0);

    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 5, 8);
    srdma_admin_set(&in, 0, 16, 16, 1);
    srdma_admin_set(&in, 1, 12, 20, 0x100);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_CREATE_EQ, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    eq = srdma_admin_get(&out, 0, 0, 16);

    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 16, 1);
    srdma_admin_set(&in, 0, 24, 5, 8);
    srdma_admin_set(&in, 1, 0, 16, eq);
    srdma_admin_set(&in, 1, 31, 1, 1);
    srdma_admin_set64(&in, 2, 0x200000);
    srdma_admin_set64(&in, 4, 0x300000);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_CREATE_CQ, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    cq = srdma_admin_get(&out, 0, 0, 24);

    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 24, pd);
    srdma_admin_set(&in, 0, 24, 8, 2);
    srdma_admin_set(&in, 1, 0, 24, cq);
    srdma_admin_set(&in, 1, 31, 1, 1);
    srdma_admin_set(&in, 2, 0, 24, cq);
    srdma_admin_set(&in, 2, 31, 1, 1);
    srdma_admin_set64(&in, 6, 0x400000);
    srdma_admin_set64(&in, 8, 0x500000);
    srdma_admin_set(&in, 8, 0, 6, 12);
    srdma_admin_set64(&in, 10, 0x600000);
    srdma_admin_set(&in, 10, 0, 6, 12);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_CREATE_QP, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    qp = srdma_admin_get(&out, 0, 0, 24);

    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 24, cq);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_DESTROY_CQ, &in, &out) ==
          SRDMA_ADMIN_RC_RESOURCE_BUSY);

    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 24, qp);
    srdma_admin_set(&in, 0, 24, 8, 1);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_RST2INIT_QP, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 24, qp);
    srdma_admin_set(&in, 1, 0, 24, 99);
    srdma_admin_set(&in, 1, 24, 5, 7);
    srdma_admin_set(&in, 2, 0, 24, 11);
    srdma_admin_set(&in, 2, 24, 3, 2);
    srdma_admin_set(&in, 2, 28, 4, 8);
    srdma_admin_set(&in, 3, 0, 8, 4);
    srdma_admin_set(&in, 3, 8, 8, 64);
    srdma_admin_set(&in, 4, 0, 20, 0x12345);
    srdma_admin_set(&in, 10, 16, 16, 4791);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_INIT2RTR_QP, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 24, qp);
    srdma_admin_set(&in, 1, 0, 24, 12);
    srdma_admin_set(&in, 1, 24, 5, 14);
    srdma_admin_set(&in, 1, 29, 3, 6);
    srdma_admin_set(&in, 2, 0, 3, 5);
    srdma_admin_set(&in, 2, 3, 3, 2);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_RTR2RTS_QP, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 24, qp);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_QUERY_QP, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    CHECK(srdma_admin_get(&out, 0, 0, 3) == 3);
    CHECK(srdma_admin_get(&out, 6, 0, 24) == 99);
    CHECK(srdma_admin_get(&out, 6, 24, 5) == 7);
    CHECK(srdma_admin_get(&out, 7, 24, 5) == 14);
    CHECK(srdma_admin_get(&out, 7, 29, 3) == 6);
    CHECK(srdma_admin_get(&out, 8, 24, 3) == 2);
    CHECK(srdma_admin_get(&out, 8, 27, 3) == 2);
    CHECK(srdma_admin_get(&out, 10, 0, 8) == 4);
    CHECK(srdma_admin_get(&out, 10, 8, 8) == 64);
    CHECK(srdma_admin_get(&out, 11, 0, 20) == 0x12345);
    CHECK(srdma_admin_get(&out, 17, 16, 16) == 4791);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_2RST_QP, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_DESTROY_QP, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);

    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 24, pd);
    srdma_admin_set(&in, 1, 0, 6, 12);
    srdma_admin_set64(&in, 2, 0x700000);
    srdma_admin_set64(&in, 4, 4096);
    srdma_admin_set64(&in, 6, 0x800000);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_REG_MR, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    mkey = out.dw[0];
    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 24, pd);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_DEALLOC_PD, &in, &out) ==
          SRDMA_ADMIN_RC_RESOURCE_BUSY);
    in.dw[0] = mkey;
    CHECK(execute(&admin, SRDMA_ADMIN_OP_DEREG_MR, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);

    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 24, cq);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_DESTROY_CQ, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 16, eq);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_DESTROY_EQ, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    memset(&in, 0, sizeof(in));
    srdma_admin_set(&in, 0, 0, 24, pd);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_DEALLOC_PD, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    srdma_admin_set(&in, 0, 0, 16, 1);
    CHECK(execute(&admin, SRDMA_ADMIN_OP_DEALLOC_UCTX, &in, &out) ==
          SRDMA_ADMIN_RC_SUCC);
    return 0;
}

static int test_doorbell_layout(void)
{
    static const uint32_t hw_ids[SRDMA_DB_TYPE_COUNT] = {
        [SRDMA_DB_ADMINQ] = SRDMA_DB_HW_ADMINQ,
        [SRDMA_DB_AEQ] = SRDMA_DB_HW_AEQ,
        [SRDMA_DB_CEQ] = SRDMA_DB_HW_CEQ,
        [SRDMA_DB_CQ] = SRDMA_DB_HW_CQ,
        [SRDMA_DB_SQ] = SRDMA_DB_HW_SQ,
        [SRDMA_DB_RQ] = SRDMA_DB_HW_RQ,
    };

    CHECK(VFIO_ADMINQ_DB_REGION_OFFSET == UINT32_C(0x8000));
    CHECK(VFIO_ADMINQ_DB_REGION_SIZE == UINT32_C(0x1000));
    CHECK(VFIO_ADMINQ_DB_LOG_SIZE == 1U);
    CHECK(VFIO_ADMINQ_DB_STRIDE_LOG_SIZE == 3U);
    CHECK(VFIO_ADMINQ_DB_COUNT == 33U);
    CHECK(SRDMA_UAR_ADMINQ_DB == 0x000U);
    CHECK(SRDMA_UAR_AEQ_DB == 0x008U);
    CHECK(SRDMA_UAR_CEQ_DB == 0x040U);
    CHECK(SRDMA_UAR_CQ_DB == 0x080U);
    CHECK(SRDMA_UAR_RQ_DB == 0x0c0U);
    CHECK(SRDMA_UAR_SQ_DB == 0x100U);
    CHECK(hw_ids[SRDMA_DB_ADMINQ] == 0U);
    CHECK(hw_ids[SRDMA_DB_AEQ] == 1U);
    CHECK(hw_ids[SRDMA_DB_CEQ] == 8U);
    CHECK(hw_ids[SRDMA_DB_CQ] == 16U);
    CHECK(hw_ids[SRDMA_DB_RQ] == 24U);
    CHECK(hw_ids[SRDMA_DB_SQ] == 32U);
    for (size_t i = 0; i < SRDMA_DB_TYPE_COUNT; i++)
        CHECK(hw_ids[i] < VFIO_ADMINQ_DB_COUNT);
    return 0;
}

int main(void)
{
    CHECK(test_admin_resources() == 0);
    CHECK(test_doorbell_layout() == 0);
    puts("srdma control tests passed");
    return 0;
}
