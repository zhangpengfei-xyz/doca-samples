#include "srdma_backend.h"
#include "srdma_admin.h"

#include "../common/vfio_adminq_abi.h"
#include "doorbell_common.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_comch_consumer.h>
#include <doca_comch_msgq.h>
#include <doca_comch_producer.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_ep.h>
#include <doca_devemu_pci_tlp.h>
#include <doca_dpa.h>
#include <doca_dma.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

DOCA_LOG_REGISTER(SRDMA_BACKEND);

#define SRDMA_DPU_DMA_INVENTORY_SIZE 2
#define SRDMA_MSIX_CAP_ID 0x11U
#define SRDMA_MSIX_CAP_OFFSET 0x40U
#define SRDMA_PCIE_CAP_ID 0x10U
#define SRDMA_PCIE_CAP_OFFSET 0x50U
#define SRDMA_PCIE_CAP_LENGTH 0x3cU
/* Host doorbells are consumed by the DPA thread and forwarded over Comch. */
#define SRDMA_DPU_BAR_ID VFIO_ADMINQ_BAR0_ID
#define SRDMA_DPU_BAR0_DB_OFFSET VFIO_ADMINQ_DB_REGION_OFFSET

extern struct doca_dpa_app *srdma_doorbell_app;
extern doca_dpa_func_t srdma_doorbell_thread;
extern doca_dpa_func_t srdma_doorbell_bind_db_rpc;
extern doca_dpa_func_t srdma_doorbell_unbind_db_rpc;

struct srdma_backend_resources {
    struct doca_dev *dev;
    struct doca_dev *dma_dev;
    struct doca_dev_rep *rep;
    struct doca_devemu_pci_type *pci_type;
    struct doca_devemu_pci_tlp_dev *tlp_dev;
    struct doca_pe *dma_pe;
    struct doca_dma *dma_ctx;
    struct doca_mmap *remote_mmap;
    struct doca_mmap *local_mmap;
    void *local_dma_buf;
    size_t local_dma_size;
    struct doca_buf_inventory *buf_inv;
    struct doca_pe *pe;
    struct doca_dpa *dpa;
    struct doca_dpa_thread *dpa_thread;
    doca_dpa_dev_uintptr_t dpa_thread_arg;
    struct doca_devemu_pci_db_completion *db_comp;
    doca_dpa_dev_devemu_pci_db_completion_t db_comp_handle;
    struct doca_devemu_pci_db *db[SRDMA_DB_TYPE_COUNT];
    doca_dpa_dev_devemu_pci_db_t db_handle[SRDMA_DB_TYPE_COUNT];
    struct doca_dpa_completion *producer_comp;
    struct doca_comch_msgq *msgq;
    struct doca_comch_consumer *host_consumer;
    struct doca_comch_producer *dpa_producer;
    doca_dpa_dev_comch_producer_t dpa_producer_handle;
    uint64_t doorbells_received;
    uint64_t adminq_tx_iova;
    uint64_t adminq_rx_iova;
    uint64_t aeq_iova;
    uint32_t adminq_tx_depth;
    uint32_t aeq_depth;
    uint16_t adminq_tx_ci;
    uint16_t adminq_rx_pi;
    uint32_t adminq_host_seq;
    uint64_t generation;
    struct doca_devemu_pci_msix *control_msix;
    struct srdma_admin admin;
    bool logger_ready;
    bool adminq_ready;
    uint32_t initial_db_pending;
    bool pci_type_started;
    bool tlp_dev_started;
    bool dpa_started;
    bool dpa_thread_started;
    bool db_comp_started;
    bool db_started[SRDMA_DB_TYPE_COUNT];
    bool producer_comp_started;
    bool msgq_started;
    bool host_consumer_started;
    bool dpa_producer_started;
};

struct srdma_dma_sync_state {
    doca_error_t result;
    uint32_t remaining_tasks;
};

static struct srdma_backend_resources g_res;

static doca_error_t process_adminq_pi(uint16_t producer);

static void log_doca_error(const char *what, doca_error_t result)
{
    DOCA_LOG_ERR("%s: %s", what, doca_error_get_descr(result));
}

static doca_error_t stop_ctx_with_progress(struct doca_ctx *ctx,
                                           struct doca_pe *pe,
                                           const char *name)
{
    doca_error_t result;
    enum doca_ctx_states state;

    result = doca_ctx_stop(ctx);
    if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS &&
        result != DOCA_ERROR_BAD_STATE) {
        log_doca_error(name, result);
        return result;
    }

    for (uint32_t i = 0; i < 10000; i++) {
        result = doca_ctx_get_state(ctx, &state);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to read context state", result);
            return result;
        }
        if (state == DOCA_CTX_STATE_IDLE) {
            return DOCA_SUCCESS;
        }
        if (pe != NULL) {
            while (doca_pe_progress(pe) != 0) {
            }
        }
        (void)nanosleep(&(const struct timespec){
                            .tv_sec = 0,
                            .tv_nsec = 1000 * 1000,
                        },
                        NULL);
    }

    DOCA_LOG_ERR("%s: context did not stop before timeout", name);
    return DOCA_ERROR_TIME_OUT;
}

void srdma_backend_default_opts(struct srdma_backend_opts *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->pci_addr = SRDMA_DPU_DEFAULT_PCI_ADDR;
    opts->pci_type_name = SRDMA_DPU_DEFAULT_PCI_TYPE_NAME;
    opts->vhca_id = SRDMA_DPU_DEFAULT_VHCA_ID;
    opts->num_db = SRDMA_DPU_DEFAULT_DB_COUNT;
    opts->local_dma_size = SRDMA_DPU_DEFAULT_LOCAL_DMA_SIZE;
}

static doca_error_t init_logging(void)
{
    doca_error_t result;
    struct doca_log_backend *sdk_log;

    if (g_res.logger_ready) {
        return DOCA_SUCCESS;
    }

    result = doca_log_backend_create_standard();
    if (result != DOCA_SUCCESS) {
        return result;
    }

    result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
    if (result != DOCA_SUCCESS) {
        return result;
    }

    result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
    if (result != DOCA_SUCCESS) {
        return result;
    }

    g_res.logger_ready = true;
    return DOCA_SUCCESS;
}

static doca_error_t find_supported_tlp_device(const char *pci_addr,
                                              struct doca_dev **dev)
{
    struct doca_devinfo **dev_list = NULL;
    uint32_t nb_devs = 0;
    doca_error_t result;
    uint16_t max_tlp_types;
    uint8_t is_equal;

    *dev = NULL;
    result = doca_devinfo_create_list(&dev_list, &nb_devs);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to load DOCA device list", result);
        return result;
    }

    for (uint32_t i = 0; i < nb_devs; i++) {
        result = doca_devinfo_is_equal_pci_addr(dev_list[i], pci_addr,
                                                &is_equal);
        if (result != DOCA_SUCCESS || !is_equal) {
            continue;
        }

        result = doca_devemu_pci_tlp_cap_get_max_types(dev_list[i],
                                                       &max_tlp_types);
        if (result != DOCA_SUCCESS || max_tlp_types == 0) {
            continue;
        }

        result = doca_dev_open(dev_list[i], dev);
        doca_devinfo_destroy_list(dev_list);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to open DOCA device", result);
        }
        return result;
    }

    doca_devinfo_destroy_list(dev_list);
    DOCA_LOG_ERR("no TLP-capable DOCA device found at %s", pci_addr);
    return DOCA_ERROR_NOT_FOUND;
}

static doca_error_t configure_and_start_pci_type(
    struct doca_devemu_pci_type *pci_type,
    struct doca_dev *dev)
{
    doca_error_t result;

    result = doca_devemu_pci_type_set_dev(pci_type, dev);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set PCI type device", result);
        return result;
    }

    result = doca_devemu_pci_type_set_num_msix(pci_type,
                                                VFIO_ADMINQ_NUM_MSIX);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_type_set_num_db(pci_type,
                                              VFIO_ADMINQ_DB_COUNT);
    if (result != DOCA_SUCCESS)
        return result;

    result = doca_devemu_pci_type_set_memory_bar_conf(
        pci_type, VFIO_ADMINQ_DOCA_BAR0_ID, VFIO_ADMINQ_DOCA_BAR0_LOG_SIZE,
        DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT, 0);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to configure BAR0", result);
        return result;
    }

    result = doca_devemu_pci_type_set_memory_bar_conf(
        pci_type, 1, 0, DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT, 0);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to configure BAR1", result);
        return result;
    }

    result = doca_devemu_pci_tlp_type_set_bar_transaction_region_conf(
        pci_type, VFIO_ADMINQ_DOCA_BAR0_ID, VFIO_ADMINQ_BAR0_CFG_OFFSET,
        VFIO_ADMINQ_BAR0_CFG_SIZE);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to configure BAR0 TLP region", result);
        return result;
    }

    result = doca_devemu_pci_type_set_bar_db_region_by_offset_conf(
        pci_type, VFIO_ADMINQ_DOCA_BAR0_ID,
        VFIO_ADMINQ_DB_REGION_OFFSET, VFIO_ADMINQ_DB_REGION_SIZE,
        VFIO_ADMINQ_DB_LOG_SIZE, VFIO_ADMINQ_DB_STRIDE_LOG_SIZE);
    if (result != DOCA_SUCCESS)
        return result;

    result = doca_devemu_pci_type_set_bar_msix_table_region_conf(
        pci_type, VFIO_ADMINQ_DOCA_BAR0_ID, VFIO_ADMINQ_MSIX_TABLE_OFFSET,
        VFIO_ADMINQ_MSIX_TABLE_SIZE);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_type_set_bar_msix_pba_region_conf(
        pci_type, VFIO_ADMINQ_DOCA_BAR0_ID, VFIO_ADMINQ_MSIX_PBA_OFFSET,
        VFIO_ADMINQ_MSIX_PBA_SIZE);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_tlp_type_set_pci_cap_conf(
        pci_type, SRDMA_MSIX_CAP_ID, SRDMA_MSIX_CAP_OFFSET, 12);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_tlp_type_set_pci_cap_conf(
        pci_type, SRDMA_PCIE_CAP_ID, SRDMA_PCIE_CAP_OFFSET,
        SRDMA_PCIE_CAP_LENGTH);
    if (result != DOCA_SUCCESS)
        return result;

    result = doca_devemu_pci_type_start(pci_type);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start PCI type", result);
        return result;
    }

    g_res.pci_type_started = true;
    return DOCA_SUCCESS;
}

static doca_error_t open_rep_by_vhca(struct doca_devemu_pci_type *pci_type,
                                      uint16_t expected_vhca_id,
                                      struct doca_dev_rep **rep)
{
    struct doca_devinfo_rep **rep_list = NULL;
    uint32_t nb_reps = 0;
    doca_error_t result;
    uint16_t vhca_id;

    *rep = NULL;
    result = doca_devemu_pci_type_create_rep_list(pci_type, &rep_list,
                                                  &nb_reps);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create representor list", result);
        return result;
    }

    DOCA_LOG_INFO("found %u representors", nb_reps);
    for (uint32_t i = 0; i < nb_reps; i++) {
        result = doca_devinfo_rep_get_vhca_id(rep_list[i], &vhca_id);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to read representor vhca_id", result);
            continue;
        }

        DOCA_LOG_INFO("representor[%u] vhca_id=%u", i, vhca_id);
        if (vhca_id != expected_vhca_id) {
            continue;
        }

        result = doca_dev_rep_open(rep_list[i], rep);
        doca_devinfo_rep_destroy_list(rep_list);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to open matching representor", result);
        }
        return result;
    }

    doca_devinfo_rep_destroy_list(rep_list);
    DOCA_LOG_ERR("no representor matched vhca_id=%u", expected_vhca_id);
    return DOCA_ERROR_NOT_FOUND;
}

static doca_error_t create_started_tlp_endpoint(uint16_t num_db)
{
    doca_error_t result;
    struct doca_devemu_pci_ep *ep;

    result = doca_devemu_pci_tlp_dev_create(g_res.pci_type, g_res.rep,
                                            &g_res.tlp_dev);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create TLP endpoint", result);
        return result;
    }

    ep = doca_devemu_pci_tlp_dev_as_ep(g_res.tlp_dev);
    if (num_db != VFIO_ADMINQ_DB_COUNT)
        return DOCA_ERROR_INVALID_VALUE;
    result = doca_devemu_pci_ep_set_num_db(ep, num_db);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to configure endpoint doorbell count", result);
        return result;
    }
    result = doca_devemu_pci_ep_set_num_msix(ep, VFIO_ADMINQ_NUM_MSIX);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to configure endpoint MSI-X count", result);
        return result;
    }

    result = doca_devemu_pci_tlp_dev_start(g_res.tlp_dev);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start TLP endpoint", result);
        return result;
    }

    g_res.tlp_dev_started = true;
    return DOCA_SUCCESS;
}

static doca_error_t setup_remote_mmap(void)
{
    doca_error_t result;

    result = doca_devemu_pci_ep_mmap_create(
        doca_devemu_pci_tlp_dev_as_ep(g_res.tlp_dev), &g_res.remote_mmap);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create remote mmap", result);
        return result;
    }

    result = doca_mmap_set_max_num_devices(g_res.remote_mmap, 1);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set remote mmap max devices", result);
        return result;
    }

    result = doca_mmap_add_dev(g_res.remote_mmap, g_res.dma_dev);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to add DMA device to remote mmap", result);
        return result;
    }

    result = doca_mmap_set_permissions(g_res.remote_mmap,
                                       DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set remote mmap permissions", result);
        return result;
    }

    result = doca_mmap_set_memrange(g_res.remote_mmap, 0, UINT64_MAX);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set remote mmap range", result);
        return result;
    }

    result = doca_mmap_start(g_res.remote_mmap);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start remote mmap", result);
        return result;
    }

    return DOCA_SUCCESS;
}

static doca_error_t setup_local_mmap(size_t local_dma_size)
{
    doca_error_t result;

    g_res.local_dma_size = local_dma_size;
    result = posix_memalign(&g_res.local_dma_buf, 4096, local_dma_size);
    if (result != 0) {
        DOCA_LOG_ERR("failed to allocate local DMA buffer: %s",
                     strerror(result));
        return DOCA_ERROR_NO_MEMORY;
    }
    memset(g_res.local_dma_buf, 0, local_dma_size);

    result = doca_mmap_create(&g_res.local_mmap);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create local mmap", result);
        return result;
    }

    result = doca_mmap_add_dev(g_res.local_mmap, g_res.dma_dev);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to add DMA device to local mmap", result);
        return result;
    }

    result = doca_mmap_set_memrange(g_res.local_mmap, g_res.local_dma_buf,
                                    local_dma_size);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set local mmap range", result);
        return result;
    }

    result = doca_mmap_start(g_res.local_mmap);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start local mmap", result);
        return result;
    }

    return DOCA_SUCCESS;
}

static void dma_completed_cb(struct doca_dma_task_memcpy *dma_task,
                             union doca_data task_user_data,
                             union doca_data ctx_user_data)
{
    struct srdma_dma_sync_state *state = task_user_data.ptr;

    (void)ctx_user_data;
    if (state != NULL) {
        state->result = DOCA_SUCCESS;
        state->remaining_tasks--;
    }
    doca_task_free(doca_dma_task_memcpy_as_task(dma_task));
}

static void dma_error_cb(struct doca_dma_task_memcpy *dma_task,
                         union doca_data task_user_data,
                         union doca_data ctx_user_data)
{
    struct doca_task *task = doca_dma_task_memcpy_as_task(dma_task);
    struct srdma_dma_sync_state *state = task_user_data.ptr;

    (void)ctx_user_data;
    if (state != NULL) {
        state->result = doca_task_get_status(task);
        state->remaining_tasks--;
    }
    doca_task_free(task);
}

static doca_error_t setup_dma(size_t local_dma_size)
{
    doca_error_t result;
    struct doca_ctx *ctx;

    result = doca_dev_open(doca_dev_as_devinfo(g_res.dev), &g_res.dma_dev);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to open DMA DOCA device", result);
        return result;
    }

    result = doca_pe_create(&g_res.dma_pe);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create DMA progress engine", result);
        return result;
    }

    result = doca_dma_create(g_res.dma_dev, &g_res.dma_ctx);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create DMA context", result);
        return result;
    }

    result = doca_dma_task_memcpy_set_conf(g_res.dma_ctx, dma_completed_cb,
                                           dma_error_cb, 1);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to configure DMA memcpy task", result);
        return result;
    }

    ctx = doca_dma_as_ctx(g_res.dma_ctx);
    result = doca_pe_connect_ctx(g_res.dma_pe, ctx);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to connect DMA context to PE", result);
        return result;
    }

    result = doca_ctx_start(ctx);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start DMA context", result);
        return result;
    }

    result = setup_remote_mmap();
    if (result != DOCA_SUCCESS) {
        return result;
    }

    result = setup_local_mmap(local_dma_size);
    if (result != DOCA_SUCCESS) {
        return result;
    }

    result = doca_buf_inventory_create(SRDMA_DPU_DMA_INVENTORY_SIZE,
                                       &g_res.buf_inv);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create DMA buffer inventory", result);
        return result;
    }

    result = doca_buf_inventory_start(g_res.buf_inv);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start DMA buffer inventory", result);
        return result;
    }

    return DOCA_SUCCESS;
}

static doca_error_t srdma_dma_copy(struct doca_mmap *src_mmap, void *src_addr,
                                   struct doca_mmap *dst_mmap, void *dst_addr,
                                   size_t len)
{
    const struct timespec sleep_time = {
        .tv_sec = 0,
        .tv_nsec = 1000 * 1000,
    };
    struct srdma_dma_sync_state sync_state = {
        .result = DOCA_ERROR_IN_PROGRESS,
        .remaining_tasks = 1,
    };
    union doca_data task_user_data = {
        .ptr = &sync_state,
    };
    struct doca_buf *src_buf = NULL;
    struct doca_buf *dst_buf = NULL;
    struct doca_dma_task_memcpy *dma_task = NULL;
    struct doca_task *task;
    doca_error_t result;

    result = doca_buf_inventory_buf_get_by_addr(g_res.buf_inv, src_mmap,
                                                src_addr, len, &src_buf);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to acquire source DMA buffer", result);
        return result;
    }

    result = doca_buf_inventory_buf_get_by_addr(g_res.buf_inv, dst_mmap,
                                                dst_addr, len, &dst_buf);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to acquire destination DMA buffer", result);
        goto out_src;
    }

    result = doca_dma_task_memcpy_alloc_init(g_res.dma_ctx, src_buf, dst_buf,
                                             task_user_data, &dma_task);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to allocate DMA memcpy task", result);
        goto out_dst;
    }

    task = doca_dma_task_memcpy_as_task(dma_task);
    result = doca_buf_set_data(src_buf, src_addr, len);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set source DMA data", result);
        doca_task_free(task);
        goto out_dst;
    }

    result = doca_task_submit(task);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to submit DMA memcpy task", result);
        doca_task_free(task);
        goto out_dst;
    }

    while (sync_state.remaining_tasks != 0) {
        while (doca_pe_progress(g_res.dma_pe) != 0) {
        }
        if (sync_state.remaining_tasks == 0) {
            break;
        }
        (void)nanosleep(&sleep_time, NULL);
    }

    result = sync_state.result;
    if (result != DOCA_SUCCESS) {
        log_doca_error("DMA memcpy completed with error", result);
    }

out_dst:
    if (dst_buf != NULL) {
        (void)doca_buf_dec_refcount(dst_buf, NULL);
    }
out_src:
    if (src_buf != NULL) {
        (void)doca_buf_dec_refcount(src_buf, NULL);
    }
    return result;
}

static void dump_adminq_text(const char *label,
                             const char text[SRDMA_ADMINQ_TEST_TEXT_LEN])
{
    printf("%s\"%.*s\"", label, SRDMA_ADMINQ_TEST_TEXT_LEN, text);
}

static doca_error_t __attribute__((unused)) srdma_backend_handle_adminq_doorbell(
    struct srdma_backend_resources *res)
{
    struct srdma_adminq_test_msg *msg = res->local_dma_buf;
    void *remote_addr;
    doca_error_t result;

    if (!res->adminq_ready || res->adminq_tx_iova == 0) {
        printf("adminq DMA skipped: no SRDMA start message has armed an "
               "adminq IOVA\n");
        return DOCA_SUCCESS;
    }
    if (res->local_dma_size < sizeof(*msg)) {
        printf("adminq DMA skipped: local DMA buffer too small: %zu < %zu\n",
               res->local_dma_size, sizeof(*msg));
        return DOCA_ERROR_INVALID_VALUE;
    }

    remote_addr = (void *)(uintptr_t)res->adminq_tx_iova;
    memset(msg, 0, sizeof(*msg));
    printf("adminq DMA read start: remote_iova=0x%" PRIx64 " len=%zu\n",
           res->adminq_tx_iova, sizeof(*msg));
    result = srdma_dma_copy(res->remote_mmap, remote_addr, res->local_mmap,
                            msg, sizeof(*msg));
    if (result != DOCA_SUCCESS) {
        return result;
    }

    printf("adminq DMA read completed: magic=0x%08x version=%u status=%u "
           "guest_seq=%u guest_iova=0x%" PRIx64 " ",
           msg->magic, msg->version, msg->status, msg->guest_seq,
           msg->guest_iova);
    dump_adminq_text("guest_text=", msg->guest_text);
    printf("\n");

    if (msg->magic != SRDMA_ADMINQ_TEST_MAGIC ||
        msg->version != SRDMA_ADMINQ_TEST_VERSION) {
        printf("adminq DMA update skipped: unexpected test message header\n");
        return DOCA_SUCCESS;
    }

    msg->status = SRDMA_ADMINQ_TEST_HOST_DONE;
    msg->host_seq = ++res->adminq_host_seq;
    msg->host_seen_doorbells = res->doorbells_received;
    snprintf(msg->host_text, sizeof(msg->host_text),
             "host dma update after doorbell %" PRIu64,
             res->doorbells_received);

    printf("adminq DMA write start: remote_iova=0x%" PRIx64 " len=%zu\n",
           res->adminq_tx_iova, sizeof(*msg));
    result = srdma_dma_copy(res->local_mmap, msg, res->remote_mmap,
                            remote_addr, sizeof(*msg));
    if (result != DOCA_SUCCESS) {
        return result;
    }

    printf("adminq DMA update completed: host_seq=%u status=%u\n",
           msg->host_seq, msg->status);
    return DOCA_SUCCESS;
}

static void doorbell_recv_cb(
    struct doca_comch_consumer_task_post_recv *recv_task,
    union doca_data task_user_data,
    union doca_data ctx_user_data)
{
    struct srdma_backend_resources *res = ctx_user_data.ptr;
    const uint8_t *imm;
    uint32_t imm_len;
    struct srdma_db_msg msg;
    struct doca_task *task;
    doca_error_t result;

    (void)task_user_data;

    imm = doca_comch_consumer_task_post_recv_get_imm_data(recv_task);
    imm_len = doca_comch_consumer_task_post_recv_get_imm_data_len(recv_task);
    if (imm != NULL && imm_len >= sizeof(msg)) {
        memcpy(&msg, imm, sizeof(msg));
        if (msg.type == SRDMA_DB_MSG_HOST_DB) {
            uint32_t db_id = (uint32_t)msg.user_data;
            uint32_t payload = msg.db_value & SRDMA_DB_VALUE_MASK;

            if (db_id >= SRDMA_DB_TYPE_COUNT) {
                DOCA_LOG_WARN("received invalid SRDMA doorbell id=%u", db_id);
            } else if ((res->initial_db_pending & (UINT32_C(1) << db_id)) != 0 &&
                       msg.db_value == 0) {
                res->initial_db_pending &= ~(UINT32_C(1) << db_id);
                DOCA_LOG_INFO("ignored initial SRDMA doorbell id=%u value=0",
                              db_id);
            } else {
                res->initial_db_pending &= ~(UINT32_C(1) << db_id);
                res->doorbells_received++;
                printf("doorbell received: db_id=%u value=0x%04x payload=0x%04x\n",
                       db_id, msg.db_value, payload);
                if (db_id == SRDMA_DB_ADMINQ) {
                    result = process_adminq_pi(payload & UINT16_MAX);
                    if (result != DOCA_SUCCESS) {
                        printf("AdminQ doorbell processing failed: %s\n",
                               doca_error_get_descr(result));
                    }
                }
                /* AEQ/CEQ/CQ/SQ/RQ are observed here; the data plane is out
                 * of scope, and CQ/SQ/RQ state remains in DMA DB records. */
                fflush(stdout);
            }
        } else {
            DOCA_LOG_WARN("received unknown SRDMA DPA message type=%u",
                          msg.type);
        }
    } else {
        DOCA_LOG_WARN("received malformed SRDMA DPA message len=%u",
                      imm_len);
    }

    task = doca_comch_consumer_task_post_recv_as_task(recv_task);
    result = doca_task_submit(task);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to resubmit doorbell receive task", result);
        doca_task_free(task);
    }
}

static void doorbell_recv_error_cb(
    struct doca_comch_consumer_task_post_recv *recv_task,
    union doca_data task_user_data,
    union doca_data ctx_user_data)
{
    struct doca_task *task;

    (void)task_user_data;
    (void)ctx_user_data;

    task = doca_comch_consumer_task_post_recv_as_task(recv_task);
    doca_task_free(task);
}

static doca_error_t submit_host_receive_tasks(void)
{
    doca_error_t result;

    for (uint32_t i = 0; i < SRDMA_DB_MAX_MSGS; i++) {
        struct doca_comch_consumer_task_post_recv *recv_task;
        struct doca_task *task;

        result = doca_comch_consumer_task_post_recv_alloc_init(
            g_res.host_consumer, NULL, &recv_task);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to allocate doorbell receive task",
                           result);
            return result;
        }

        task = doca_comch_consumer_task_post_recv_as_task(recv_task);
        result = doca_task_submit(task);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to submit doorbell receive task", result);
            doca_task_free(task);
            return result;
        }
    }

    return DOCA_SUCCESS;
}

static doca_error_t setup_dpa_context(void)
{
    struct srdma_dpa_thread_arg thread_arg;
    doca_error_t result;

    result = doca_dpa_create(g_res.dev, &g_res.dpa);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create DPA context", result);
        return result;
    }

    result = doca_dpa_set_app(g_res.dpa, srdma_doorbell_app);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set DPA doorbell app", result);
        return result;
    }

    result = doca_dpa_start(g_res.dpa);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start DPA context", result);
        return result;
    }
    g_res.dpa_started = true;

    result = doca_dpa_mem_alloc(g_res.dpa, sizeof(thread_arg),
                                &g_res.dpa_thread_arg);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to allocate DPA thread argument", result);
        return result;
    }

    result = doca_dpa_thread_create(g_res.dpa, &g_res.dpa_thread);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create DPA thread", result);
        return result;
    }

    result = doca_dpa_thread_set_func_arg(g_res.dpa_thread,
                                          srdma_doorbell_thread,
                                          g_res.dpa_thread_arg);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set DPA thread function", result);
        return result;
    }

    result = doca_dpa_thread_start(g_res.dpa_thread);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start DPA thread", result);
        return result;
    }
    g_res.dpa_thread_started = true;

    result = doca_devemu_pci_db_completion_create(g_res.dpa_thread,
                                                  &g_res.db_comp);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create DB completion", result);
        return result;
    }

    result = doca_devemu_pci_db_completion_set_max_num_dbs(
        g_res.db_comp, SRDMA_DB_TYPE_COUNT);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set DB completion capacity", result);
        return result;
    }

    result = doca_devemu_pci_db_completion_start(g_res.db_comp);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start DB completion", result);
        return result;
    }
    g_res.db_comp_started = true;

    result = doca_devemu_pci_db_completion_get_dpa_handle(
        g_res.db_comp, &g_res.db_comp_handle);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to get DB completion DPA handle", result);
        return result;
    }

    result = doca_dpa_completion_create(g_res.dpa, SRDMA_DB_MAX_MSGS,
                                        &g_res.producer_comp);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create DPA producer completion", result);
        return result;
    }

    result = doca_dpa_completion_set_thread(g_res.producer_comp,
                                            g_res.dpa_thread);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to attach producer completion to DPA thread",
                       result);
        return result;
    }

    result = doca_dpa_completion_start(g_res.producer_comp);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start DPA producer completion", result);
        return result;
    }
    g_res.producer_comp_started = true;

    return DOCA_SUCCESS;
}

static doca_error_t setup_host_msgq(void)
{
    struct doca_ctx *consumer_ctx;
    struct doca_ctx *producer_ctx;
    union doca_data user_data;
    doca_error_t result;

    result = doca_pe_create(&g_res.pe);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create doorbell progress engine", result);
        return result;
    }

    result = doca_comch_msgq_create(g_res.dev, &g_res.msgq);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create doorbell MsgQ", result);
        return result;
    }

    result = doca_comch_msgq_set_max_num_consumers(g_res.msgq, 1);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set MsgQ consumer count", result);
        return result;
    }

    result = doca_comch_msgq_set_max_num_producers(g_res.msgq, 1);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set MsgQ producer count", result);
        return result;
    }

    result = doca_comch_msgq_set_dpa_producer(g_res.msgq, g_res.dpa);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set MsgQ DPA producer", result);
        return result;
    }

    result = doca_comch_msgq_start(g_res.msgq);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start doorbell MsgQ", result);
        return result;
    }
    g_res.msgq_started = true;

    result = doca_comch_msgq_consumer_create(g_res.msgq,
                                             &g_res.host_consumer);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create host MsgQ consumer", result);
        return result;
    }

    consumer_ctx = doca_comch_consumer_as_ctx(g_res.host_consumer);
    result = doca_comch_consumer_set_imm_data_len(g_res.host_consumer,
                                                  sizeof(struct srdma_db_msg));
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set host consumer immediate length",
                       result);
        return result;
    }

    user_data.ptr = &g_res;
    result = doca_ctx_set_user_data(consumer_ctx, user_data);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set host consumer user data", result);
        return result;
    }

    result = doca_pe_connect_ctx(g_res.pe, consumer_ctx);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to connect host consumer to PE", result);
        return result;
    }

    result = doca_comch_consumer_task_post_recv_set_conf(
        g_res.host_consumer, doorbell_recv_cb, doorbell_recv_error_cb,
        SRDMA_DB_MAX_MSGS);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to configure host consumer receive tasks",
                       result);
        return result;
    }

    result = doca_ctx_start(consumer_ctx);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start host consumer context", result);
        return result;
    }
    g_res.host_consumer_started = true;

    result = submit_host_receive_tasks();
    if (result != DOCA_SUCCESS) {
        return result;
    }

    result = doca_comch_msgq_producer_create(g_res.msgq,
                                             &g_res.dpa_producer);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create DPA MsgQ producer", result);
        return result;
    }

    producer_ctx = doca_comch_producer_as_ctx(g_res.dpa_producer);
    result = doca_ctx_set_datapath_on_dpa(producer_ctx, g_res.dpa);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set producer datapath on DPA", result);
        return result;
    }

    result = doca_comch_producer_set_dev_max_num_send(g_res.dpa_producer,
                                                      SRDMA_DB_MAX_MSGS);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to set DPA producer send depth", result);
        return result;
    }

    result = doca_comch_producer_dpa_completion_attach(
        g_res.dpa_producer, g_res.producer_comp);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to attach DPA producer completion", result);
        return result;
    }

    result = doca_ctx_start(producer_ctx);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start DPA producer context", result);
        return result;
    }
    g_res.dpa_producer_started = true;

    result = doca_comch_producer_get_dpa_handle(g_res.dpa_producer,
                                                &g_res.dpa_producer_handle);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to get DPA producer handle", result);
        return result;
    }

    return DOCA_SUCCESS;
}

static doca_error_t run_dpa_doorbell_thread(void)
{
    struct srdma_dpa_thread_arg thread_arg = {
        .dpa_db_comp = g_res.db_comp_handle,
        .dpa_producer = g_res.dpa_producer_handle,
    };
    doca_error_t result;

    result = doca_dpa_h2d_memcpy(g_res.dpa, g_res.dpa_thread_arg,
                                 &thread_arg, sizeof(thread_arg));
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to copy DPA thread argument", result);
        return result;
    }

    result = doca_dpa_thread_run(g_res.dpa_thread);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to run DPA doorbell thread", result);
        return result;
    }

    return DOCA_SUCCESS;
}

static doca_error_t setup_doorbell_transport(void)
{
    doca_error_t result;

    result = setup_dpa_context();
    if (result != DOCA_SUCCESS) {
        return result;
    }

    result = setup_host_msgq();
    if (result != DOCA_SUCCESS) {
        return result;
    }

    return run_dpa_doorbell_thread();
}

static uint32_t doorbell_hw_id(uint32_t db_id)
{
    static const uint32_t hw_ids[SRDMA_DB_TYPE_COUNT] = {
        [SRDMA_DB_ADMINQ] = SRDMA_DB_HW_ADMINQ,
        [SRDMA_DB_AEQ] = SRDMA_DB_HW_AEQ,
        [SRDMA_DB_CEQ] = SRDMA_DB_HW_CEQ,
        [SRDMA_DB_CQ] = SRDMA_DB_HW_CQ,
        [SRDMA_DB_SQ] = SRDMA_DB_HW_SQ,
        [SRDMA_DB_RQ] = SRDMA_DB_HW_RQ,
    };

    return hw_ids[db_id];
}

static doca_error_t create_doorbell(uint32_t db_id)
{
    struct doca_devemu_pci_ep *ep;
    uint64_t rpc_ret;
    doca_error_t result;

    ep = doca_devemu_pci_tlp_dev_as_ep(g_res.tlp_dev);
    result = doca_devemu_pci_ep_create_db_on_dpa(
        ep, g_res.db_comp, SRDMA_DPU_BAR_ID, SRDMA_DPU_BAR0_DB_OFFSET,
        doorbell_hw_id(db_id), db_id, &g_res.db[db_id]);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create SRDMA doorbell on DPA", result);
        return result;
    }

    result = doca_devemu_pci_db_get_dpa_handle(g_res.db[db_id],
                                                &g_res.db_handle[db_id]);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to get SRDMA doorbell DPA handle", result);
        return result;
    }

    result = doca_dpa_rpc(g_res.dpa, &srdma_doorbell_bind_db_rpc, &rpc_ret,
                          g_res.db_comp_handle, g_res.db_handle[db_id]);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to bind SRDMA doorbell on DPA", result);
        return result;
    }
    if (rpc_ret != SRDMA_DB_RPC_SUCCESS) {
        DOCA_LOG_ERR("DPA rejected SRDMA doorbell bind");
        return DOCA_ERROR_BAD_STATE;
    }

    g_res.initial_db_pending |= UINT32_C(1) << db_id;
    result = doca_devemu_pci_db_start(g_res.db[db_id]);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to start SRDMA doorbell", result);
        return result;
    }
    g_res.db_started[db_id] = true;

    DOCA_LOG_INFO("SRDMA doorbell listening on BAR%u region=0x%x db_id=%u hw_id=%u",
                  SRDMA_DPU_BAR_ID, SRDMA_DPU_BAR0_DB_OFFSET, db_id,
                  doorbell_hw_id(db_id));
    return DOCA_SUCCESS;
}

static doca_error_t create_doorbells(void)
{
    doca_error_t result;

    for (uint32_t db_id = 0; db_id < SRDMA_DB_TYPE_COUNT; db_id++) {
        result = create_doorbell(db_id);
        if (result != DOCA_SUCCESS)
            return result;
    }
    return DOCA_SUCCESS;
}

doca_error_t srdma_backend_init(const struct srdma_backend_opts *opts)
{
    doca_error_t result;

    if (opts == NULL || opts->pci_addr == NULL || opts->pci_type_name == NULL ||
        opts->local_dma_size < SRDMA_ADMIN_ENTRY_SIZE ||
        opts->num_db != VFIO_ADMINQ_DB_COUNT) {
        return DOCA_ERROR_INVALID_VALUE;
    }

    result = init_logging();
    if (result != DOCA_SUCCESS) {
        return result;
    }

    DOCA_LOG_INFO("initializing SRDMA backend: pci=%s type=%s vhca_id=%u",
                  opts->pci_addr, opts->pci_type_name, opts->vhca_id);
    g_res.doorbells_received = 0;
    g_res.adminq_tx_iova = 0;
    g_res.adminq_rx_iova = 0;
    g_res.aeq_iova = 0;
    g_res.adminq_tx_depth = 0;
    g_res.adminq_tx_ci = 0;
    g_res.adminq_rx_pi = 0;
    g_res.generation = opts->generation;
    g_res.adminq_ready = false;
    g_res.initial_db_pending = 0;
    srdma_admin_init(&g_res.admin, (uint32_t)g_res.generation);

    result = find_supported_tlp_device(opts->pci_addr, &g_res.dev);
    if (result != DOCA_SUCCESS) {
        goto fail;
    }

    result = doca_devemu_pci_tlp_type_create(opts->pci_type_name,
                                             &g_res.pci_type);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create PCI TLP type", result);
        goto fail;
    }

    result = configure_and_start_pci_type(g_res.pci_type, g_res.dev);
    if (result != DOCA_SUCCESS) {
        goto fail;
    }

    result = open_rep_by_vhca(g_res.pci_type, opts->vhca_id, &g_res.rep);
    if (result != DOCA_SUCCESS) {
        goto fail;
    }

    result = create_started_tlp_endpoint(opts->num_db);
    if (result != DOCA_SUCCESS) {
        goto fail;
    }

    result = setup_dma(opts->local_dma_size);
    if (result != DOCA_SUCCESS) {
        goto fail;
    }

    result = setup_doorbell_transport();
    if (result != DOCA_SUCCESS)
        goto fail;

    result = create_doorbells();
    if (result != DOCA_SUCCESS)
        goto fail;

    result = doca_devemu_pci_ep_create_msix(
        doca_devemu_pci_tlp_dev_as_ep(g_res.tlp_dev),
        VFIO_ADMINQ_DOCA_BAR0_ID, VFIO_ADMINQ_MSIX_TABLE_OFFSET, 0,
        &g_res.control_msix);
    if (result != DOCA_SUCCESS) {
        log_doca_error("failed to create control MSI-X", result);
        goto fail;
    }

    DOCA_LOG_INFO("SRDMA backend initialized successfully for vhca_id=%u",
                  opts->vhca_id);
    return DOCA_SUCCESS;

fail:
    srdma_backend_cleanup();
    return result;
}

void srdma_backend_set_adminq(uint64_t txq_iova, uint64_t rxq_iova,
                              uint64_t aeq_iova, uint32_t txq_depth,
                              uint32_t aeq_depth)
{
    g_res.adminq_tx_iova = txq_iova;
    g_res.adminq_rx_iova = rxq_iova;
    g_res.aeq_iova = aeq_iova;
    g_res.adminq_tx_depth = txq_depth;
    g_res.aeq_depth = aeq_depth;
    g_res.adminq_tx_ci = 0;
    g_res.adminq_rx_pi = 0;
    g_res.adminq_ready = txq_iova != 0 && rxq_iova != 0 && aeq_iova != 0 &&
                         txq_depth == SRDMA_ADMINQ_DEPTH &&
                         aeq_depth == SRDMA_AEQ_DEPTH;

    if (g_res.adminq_ready) {
        printf("srdma AdminQ armed: tx=0x%" PRIx64 " rx=0x%" PRIx64
               " aeq=0x%" PRIx64 " generation=%" PRIu64 "\n",
               txq_iova, rxq_iova, aeq_iova, g_res.generation);
    } else {
        printf("srdma adminq DMA target cleared\n");
    }
}

void srdma_backend_clear_adminq(void)
{
    srdma_backend_set_adminq(0, 0, 0, 0, 0);
    srdma_admin_reset(&g_res.admin, (uint32_t)(g_res.generation + 1));
}

static doca_error_t process_adminq_pi(uint16_t producer)
{
    struct srdma_admin_entry request;
    struct srdma_admin_entry response;
    uint16_t outstanding = producer - g_res.adminq_tx_ci;
    doca_error_t result;

    if (!g_res.adminq_ready)
        return DOCA_ERROR_BAD_STATE;
    if (outstanding > SRDMA_ADMINQ_DEPTH) {
        DOCA_LOG_ERR("invalid AdminQ PI: pi=%u ci=%u", producer,
                     g_res.adminq_tx_ci);
        g_res.adminq_ready = false;
        return DOCA_ERROR_BAD_STATE;
    }

    while (g_res.adminq_tx_ci != producer) {
        uint64_t tx_addr = g_res.adminq_tx_iova +
            ((uint64_t)(g_res.adminq_tx_ci & (SRDMA_ADMINQ_DEPTH - 1U)) *
             SRDMA_ADMIN_ENTRY_SIZE);
        uint64_t rx_addr = g_res.adminq_rx_iova +
            ((uint64_t)(g_res.adminq_rx_pi & (SRDMA_ADMINQ_DEPTH - 1U)) *
             SRDMA_ADMIN_ENTRY_SIZE);
        uint64_t rx_header;
        uint8_t id;
        uint8_t opcode;
        enum srdma_admin_rc rc;

        result = srdma_dma_copy(g_res.remote_mmap,
                                (void *)(uintptr_t)rx_addr,
                                g_res.local_mmap, g_res.local_dma_buf,
                                sizeof(rx_header));
        if (result != DOCA_SUCCESS)
            return result;
        memcpy(&rx_header, g_res.local_dma_buf, sizeof(rx_header));
        if ((rx_header >> 63) != 0) {
            DOCA_LOG_ERR("AdminQ RX ring is full at index=%u",
                         g_res.adminq_rx_pi);
            g_res.adminq_ready = false;
            return DOCA_ERROR_BAD_STATE;
        }

        result = srdma_dma_copy(g_res.remote_mmap,
                                (void *)(uintptr_t)tx_addr,
                                g_res.local_mmap, g_res.local_dma_buf,
                                sizeof(request));
        if (result != DOCA_SUCCESS)
            return result;
        memcpy(&request, g_res.local_dma_buf, sizeof(request));
        id = SRDMA_ADMIN_HDR_ID(request.hdr);
        opcode = SRDMA_ADMIN_HDR_OPCODE(request.hdr);
        g_res.adminq_tx_ci++;
        if (id >= SRDMA_ADMINQ_DEPTH ||
            SRDMA_ADMIN_HDR_SIZE(request.hdr) != SRDMA_ADMIN_ENTRY_SIZE ||
            (request.hdr >> 24) != 0) {
            memset(&response, 0, sizeof(response));
            rc = SRDMA_ADMIN_RC_INVALID_ARG;
        } else {
            rc = srdma_admin_execute(&g_res.admin, opcode, &request,
                                     &response);
        }
        response.hdr = SRDMA_ADMIN_RSP_HEADER(id, g_res.adminq_tx_ci, rc, 0);
        memcpy(g_res.local_dma_buf, &response, sizeof(response));
        result = srdma_dma_copy(g_res.local_mmap, g_res.local_dma_buf,
                                g_res.remote_mmap,
                                (void *)(uintptr_t)rx_addr,
                                sizeof(response));
        if (result != DOCA_SUCCESS)
            return result;

        response.hdr |= UINT64_C(1) << 63;
        memcpy(g_res.local_dma_buf, &response.hdr, sizeof(response.hdr));
        result = srdma_dma_copy(g_res.local_mmap, g_res.local_dma_buf,
                                g_res.remote_mmap,
                                (void *)(uintptr_t)rx_addr,
                                sizeof(response.hdr));
        if (result != DOCA_SUCCESS)
            return result;
        g_res.adminq_rx_pi++;
        doca_devemu_pci_msix_raise(g_res.control_msix);
    }
    return DOCA_SUCCESS;
}

doca_error_t srdma_backend_progress(void)
{
    if (g_res.pe == NULL) {
        return DOCA_SUCCESS;
    }

    while (doca_pe_progress(g_res.pe) != 0) {
    }

    return DOCA_SUCCESS;
}

void srdma_backend_cleanup(void)
{
    doca_error_t result;

    g_res.adminq_ready = false;
    g_res.adminq_tx_iova = 0;
    g_res.adminq_rx_iova = 0;
    g_res.aeq_iova = 0;
    g_res.adminq_tx_depth = 0;
    g_res.adminq_host_seq = 0;

    if (g_res.control_msix != NULL) {
        result = doca_devemu_pci_msix_destroy(g_res.control_msix);
        if (result != DOCA_SUCCESS)
            log_doca_error("failed to destroy control MSI-X", result);
        g_res.control_msix = NULL;
    }
    for (uint32_t db_id = SRDMA_DB_TYPE_COUNT; db_id-- > 0;) {
        if (g_res.db[db_id] == NULL)
            continue;
        if (g_res.db_started[db_id]) {
            result = doca_devemu_pci_db_stop(g_res.db[db_id]);
            if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
                log_doca_error("failed to stop doorbell", result);
            g_res.db_started[db_id] = false;
        }
        if (g_res.dpa != NULL && g_res.db_comp_handle != 0 &&
            g_res.db_handle[db_id] != 0) {
            uint64_t rpc_ret;

            result = doca_dpa_rpc(g_res.dpa, &srdma_doorbell_unbind_db_rpc,
                                  &rpc_ret, g_res.db_comp_handle,
                                  g_res.db_handle[db_id]);
            if (result != DOCA_SUCCESS)
                log_doca_error("failed to unbind doorbell on DPA", result);
            else if (rpc_ret != SRDMA_DB_RPC_SUCCESS)
                DOCA_LOG_WARN("DPA rejected SRDMA doorbell %u unbind", db_id);
        }
        result = doca_devemu_pci_db_destroy(g_res.db[db_id]);
        if (result != DOCA_SUCCESS)
            log_doca_error("failed to destroy doorbell", result);
        g_res.db[db_id] = NULL;
        g_res.db_handle[db_id] = 0;
    }

    if (g_res.dpa_producer != NULL) {
        struct doca_ctx *ctx = doca_comch_producer_as_ctx(g_res.dpa_producer);

        if (g_res.dpa_producer_started) {
            (void)stop_ctx_with_progress(ctx, NULL,
                                         "failed to stop DPA producer context");
            g_res.dpa_producer_started = false;
        }
        result = doca_comch_producer_destroy(g_res.dpa_producer);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy DPA producer", result);
        }
        g_res.dpa_producer = NULL;
        g_res.dpa_producer_handle = 0;
    }

    if (g_res.host_consumer != NULL) {
        struct doca_ctx *ctx = doca_comch_consumer_as_ctx(g_res.host_consumer);

        if (g_res.host_consumer_started) {
            (void)stop_ctx_with_progress(ctx, g_res.pe,
                                         "failed to stop host consumer context");
            g_res.host_consumer_started = false;
        }
        result = doca_comch_consumer_destroy(g_res.host_consumer);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy host consumer", result);
        }
        g_res.host_consumer = NULL;
    }

    if (g_res.msgq != NULL) {
        if (g_res.msgq_started) {
            result = doca_comch_msgq_stop(g_res.msgq);
            if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
                log_doca_error("failed to stop doorbell MsgQ", result);
            }
            g_res.msgq_started = false;
        }
        result = doca_comch_msgq_destroy(g_res.msgq);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy doorbell MsgQ", result);
        }
        g_res.msgq = NULL;
    }

    if (g_res.producer_comp != NULL) {
        if (g_res.producer_comp_started) {
            result = doca_dpa_completion_stop(g_res.producer_comp);
            if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
                log_doca_error("failed to stop DPA producer completion",
                               result);
            }
            g_res.producer_comp_started = false;
        }
        result = doca_dpa_completion_destroy(g_res.producer_comp);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy DPA producer completion",
                           result);
        }
        g_res.producer_comp = NULL;
    }

    if (g_res.db_comp != NULL) {
        if (g_res.db_comp_started) {
            result = doca_devemu_pci_db_completion_stop(g_res.db_comp);
            if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
                log_doca_error("failed to stop DB completion", result);
            }
            g_res.db_comp_started = false;
        }
        result = doca_devemu_pci_db_completion_destroy(g_res.db_comp);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy DB completion", result);
        }
        g_res.db_comp = NULL;
        g_res.db_comp_handle = 0;
    }

    if (g_res.dpa_thread != NULL) {
        if (g_res.dpa_thread_started) {
            result = doca_dpa_thread_stop(g_res.dpa_thread);
            if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
                log_doca_error("failed to stop DPA thread", result);
            }
            g_res.dpa_thread_started = false;
        }
        result = doca_dpa_thread_destroy(g_res.dpa_thread);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy DPA thread", result);
        }
        g_res.dpa_thread = NULL;
    }

    if (g_res.dpa_thread_arg != 0 && g_res.dpa != NULL) {
        result = doca_dpa_mem_free(g_res.dpa, g_res.dpa_thread_arg);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to free DPA thread argument", result);
        }
        g_res.dpa_thread_arg = 0;
    }

    if (g_res.dpa != NULL) {
        if (g_res.dpa_started) {
            result = doca_dpa_stop(g_res.dpa);
            if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
                log_doca_error("failed to stop DPA context", result);
            }
            g_res.dpa_started = false;
        }
        result = doca_dpa_destroy(g_res.dpa);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy DPA context", result);
        }
        g_res.dpa = NULL;
    }

    if (g_res.pe != NULL) {
        result = doca_pe_destroy(g_res.pe);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy doorbell PE", result);
        }
        g_res.pe = NULL;
    }

    if (g_res.buf_inv != NULL) {
        result = doca_buf_inventory_destroy(g_res.buf_inv);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy buffer inventory", result);
        }
        g_res.buf_inv = NULL;
    }

    if (g_res.local_mmap != NULL) {
        result = doca_mmap_stop(g_res.local_mmap);
        if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
            log_doca_error("failed to stop local mmap", result);
        }
        result = doca_mmap_destroy(g_res.local_mmap);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy local mmap", result);
        }
        g_res.local_mmap = NULL;
    }

    if (g_res.local_dma_buf != NULL) {
        free(g_res.local_dma_buf);
        g_res.local_dma_buf = NULL;
        g_res.local_dma_size = 0;
    }

    if (g_res.remote_mmap != NULL) {
        result = doca_mmap_stop(g_res.remote_mmap);
        if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
            log_doca_error("failed to stop remote mmap", result);
        }
        result = doca_mmap_destroy(g_res.remote_mmap);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy remote mmap", result);
        }
        g_res.remote_mmap = NULL;
    }

    if (g_res.dma_ctx != NULL) {
        struct doca_ctx *ctx = doca_dma_as_ctx(g_res.dma_ctx);
        result = doca_ctx_stop(ctx);
        if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
            log_doca_error("failed to stop DMA context", result);
        }
        result = doca_dma_destroy(g_res.dma_ctx);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy DMA context", result);
        }
        g_res.dma_ctx = NULL;
    }

    if (g_res.dma_pe != NULL) {
        result = doca_pe_destroy(g_res.dma_pe);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy DMA PE", result);
        }
        g_res.dma_pe = NULL;
    }

    if (g_res.dma_dev != NULL) {
        result = doca_dev_close(g_res.dma_dev);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to close DMA DOCA device", result);
        }
        g_res.dma_dev = NULL;
    }

    if (g_res.tlp_dev != NULL) {
        if (g_res.tlp_dev_started) {
            result = doca_devemu_pci_tlp_dev_stop(g_res.tlp_dev);
            if (result != DOCA_SUCCESS) {
                log_doca_error("failed to stop TLP endpoint", result);
            }
        }
        result = doca_devemu_pci_tlp_dev_destroy(g_res.tlp_dev);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy TLP endpoint", result);
        }
        g_res.tlp_dev = NULL;
        g_res.tlp_dev_started = false;
    }

    if (g_res.rep != NULL) {
        result = doca_dev_rep_close(g_res.rep);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to close representor", result);
        }
        g_res.rep = NULL;
    }

    if (g_res.pci_type != NULL) {
        if (g_res.pci_type_started) {
            result = doca_devemu_pci_type_stop(g_res.pci_type);
            if (result != DOCA_SUCCESS) {
                log_doca_error("failed to stop PCI type", result);
            }
        }
        result = doca_devemu_pci_type_destroy(g_res.pci_type);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to destroy PCI type", result);
        }
        g_res.pci_type = NULL;
        g_res.pci_type_started = false;
    }

    if (g_res.dev != NULL) {
        result = doca_dev_close(g_res.dev);
        if (result != DOCA_SUCCESS) {
            log_doca_error("failed to close DOCA device", result);
        }
        g_res.dev = NULL;
    }
}
