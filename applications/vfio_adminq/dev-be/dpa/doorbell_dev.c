#include <doca_dpa_dev.h>
#include <doca_dpa_dev_comch_msgq.h>
#include <doca_dpa_dev_devemu_pci.h>

#include "doorbell_common.h"

#define SRDMA_DPA_DB_BATCH 64

struct srdma_db_completion {
    doca_dpa_dev_devemu_pci_db_t db;
    doca_dpa_dev_uintptr_t user_data;
};

__dpa_rpc__ uint64_t
srdma_doorbell_bind_db_rpc(doca_dpa_dev_devemu_pci_db_completion_t db_comp,
                           doca_dpa_dev_devemu_pci_db_t db)
{
    if (doca_dpa_dev_devemu_pci_db_completion_bind_db(db_comp, db) < 0) {
        DOCA_DPA_DEV_LOG_ERR("failed to bind SRDMA doorbell to completion\n");
        return SRDMA_DB_RPC_ERROR;
    }

    return SRDMA_DB_RPC_SUCCESS;
}

__dpa_rpc__ uint64_t
srdma_doorbell_unbind_db_rpc(doca_dpa_dev_devemu_pci_db_completion_t db_comp,
                             doca_dpa_dev_devemu_pci_db_t db)
{
    if (doca_dpa_dev_devemu_pci_db_completion_unbind_db(db_comp, db) < 0) {
        DOCA_DPA_DEV_LOG_ERR("failed to unbind SRDMA doorbell\n");
        return SRDMA_DB_RPC_ERROR;
    }

    return SRDMA_DB_RPC_SUCCESS;
}

static void srdma_doorbell_send_msg(doca_dpa_dev_comch_producer_t producer,
                                    const struct srdma_db_completion *db_comp)
{
    doca_dpa_dev_devemu_pci_db_request_notification(db_comp->db);

    struct srdma_db_msg msg = {
        .type = SRDMA_DB_MSG_HOST_DB,
        .db_value = doca_dpa_dev_devemu_pci_db_get_value(db_comp->db),
        .user_data = db_comp->user_data,
    };

    (void)doca_dpa_dev_comch_producer_post_send_imm_only(
        producer, 1, (const uint8_t *)&msg, sizeof(msg),
        DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH |
            DOCA_DPA_DEV_SUBMIT_FLAG_OPTIMIZE_REPORTS);
}

__dpa_global__ void srdma_doorbell_thread(uint64_t thread_arg_raw)
{
    struct srdma_dpa_thread_arg *thread_arg =
        (struct srdma_dpa_thread_arg *)thread_arg_raw;
    doca_dpa_dev_devemu_pci_db_completion_t db_comp =
        thread_arg->dpa_db_comp;
    struct srdma_db_completion dbs[SRDMA_DPA_DB_BATCH];
    doca_dpa_dev_devemu_pci_db_completion_element_t comp;
    uint32_t num_dbs = 0;

    while (num_dbs < SRDMA_DPA_DB_BATCH &&
           doca_dpa_dev_devemu_pci_get_db_completion(db_comp, &comp) != 0) {
        doca_dpa_dev_devemu_pci_db_completion_element_get_db_properties(
            db_comp, comp, &dbs[num_dbs].db, &dbs[num_dbs].user_data);
        num_dbs++;
    }

    if (num_dbs != 0) {
        (void)doca_dpa_dev_devemu_pci_db_completion_ack(db_comp, num_dbs);
        (void)doca_dpa_dev_devemu_pci_db_completion_request_notification(
            db_comp);

        for (uint32_t i = 0; i < num_dbs; i++) {
            srdma_doorbell_send_msg(thread_arg->dpa_producer, &dbs[i]);
        }
    }

    doca_dpa_dev_thread_reschedule();
}
