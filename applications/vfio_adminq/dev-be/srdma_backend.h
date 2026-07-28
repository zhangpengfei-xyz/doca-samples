#pragma once

#include <stddef.h>
#include <stdint.h>

#include <doca_error.h>

#include "../common/vfio_adminq_abi.h"

#define SRDMA_DPU_DEFAULT_PCI_ADDR VFIO_ADMINQ_DEFAULT_DOCA_PCI_ADDR
#define SRDMA_DPU_DEFAULT_PCI_TYPE_NAME VFIO_ADMINQ_PCI_TYPE_NAME
#define SRDMA_DPU_DEFAULT_VHCA_ID 7
#define SRDMA_DPU_DEFAULT_DB_COUNT VFIO_ADMINQ_DB_COUNT
#define SRDMA_DPU_DEFAULT_LOCAL_DMA_SIZE 4096

struct srdma_backend_opts {
    const char *pci_addr;
    const char *pci_type_name;
    uint16_t vhca_id;
    uint16_t num_db;
    size_t local_dma_size;
    uint32_t timeout_sec;
    uint64_t generation;
};

void srdma_backend_default_opts(struct srdma_backend_opts *opts);
doca_error_t srdma_backend_init(const struct srdma_backend_opts *opts);
void srdma_backend_set_adminq(uint64_t txq_iova, uint64_t rxq_iova,
                              uint64_t aeq_iova, uint32_t txq_depth,
                              uint32_t aeq_depth);
void srdma_backend_clear_adminq(void);
doca_error_t srdma_backend_progress(void);
void srdma_backend_cleanup(void);
