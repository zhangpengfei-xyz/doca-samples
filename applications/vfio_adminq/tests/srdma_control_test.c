#include "../common/srdma_uar_ipc.h"
#include "../dev-be/srdma_admin.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static int test_uar_ipc(void)
{
    struct srdma_uar_ipc producer;
    struct srdma_uar_ipc consumer;
    struct srdma_uar_event event = {
        .endpoint_id = 3,
        .uctx_id = 4,
        .offset = 0x80,
        .width = 8,
        .value = UINT64_C(0x1122334455667788),
        .generation = 9,
    };
    struct srdma_uar_event received;
    char path[80];
    uint64_t wakeups;

    snprintf(path, sizeof(path), "/tmp/srdma-uar-test-%ld", (long)getpid());
    CHECK(srdma_uar_ipc_producer_init(&producer, path) == 0);
    CHECK(srdma_uar_ipc_consumer_init(&consumer, path) == 0);
    CHECK(srdma_uar_ipc_producer_progress(&producer) == 1);
    CHECK(srdma_uar_ipc_push(&producer, &event) == 0);
    CHECK(read(srdma_uar_ipc_event_fd(&consumer), &wakeups,
               sizeof(wakeups)) == (ssize_t)sizeof(wakeups));
    CHECK(wakeups == 1);
    CHECK(srdma_uar_ipc_pop(&consumer, &received) == 1);
    CHECK(memcmp(&event, &received, sizeof(event)) == 0);
    CHECK(srdma_uar_ipc_pop(&consumer, &received) == 0);
    for (unsigned int i = 0; i < SRDMA_UAR_IPC_DEPTH; i++)
        CHECK(srdma_uar_ipc_push(&producer, &event) == 0);
    CHECK(srdma_uar_ipc_push(&producer, &event) == -ENOSPC);
    CHECK(srdma_uar_ipc_is_fatal(&consumer));
    srdma_uar_ipc_cleanup(&consumer);
    srdma_uar_ipc_cleanup(&producer);
    return 0;
}

int main(void)
{
    CHECK(test_admin_resources() == 0);
    CHECK(test_uar_ipc() == 0);
    puts("srdma control tests passed");
    return 0;
}
