#define _GNU_SOURCE

#include "pci_fe.h"
#include "../common/vfio_adminq_abi.h"

#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <doca_bitfield.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_ep.h>
#include <doca_devemu_pci_info.h>
#include <doca_devemu_pci_tlp.h>
#include <doca_log.h>
#include <doca_pe.h>

DOCA_LOG_REGISTER(VFIO_ADMINQ_PCI_FE);

#define PCI_COMMAND_OFFSET 0x04U
#define PCI_COMMAND_MEMORY 0x0002U
#define PCI_COMMAND_MASTER 0x0004U
#define PCI_BAR0_LOW_OFFSET 0x10U
#define PCI_BAR0_HIGH_OFFSET 0x14U
#define PCI_SUBSYSTEM_OFFSET 0x2cU
#define PCI_CAP_PTR_OFFSET 0x34U
#define PCI_MSIX_CAP_OFFSET 0x40U
#define PCI_MSIX_CAP_ID 0x11U
#define PCI_EXP_CAP_OFFSET 0x50U
#define PCI_EXP_CAP_ID 0x10U
#define PCI_EXP_CAP_LENGTH 0x3cU
#define PCI_EXP_DEVCAP_OFFSET (PCI_EXP_CAP_OFFSET + 4U)
#define PCI_EXP_DEVCTL_OFFSET (PCI_EXP_CAP_OFFSET + 8U)
#define PCI_EXP_DEVCAP_FLR (1U << 28)
#define PCI_EXP_DEVCTL_FLR (1U << 15)
#define PCI_MSIX_MSGCTRL_ENABLE (1U << 31)
#define PCI_MSIX_MSGCTRL_MASKALL (1U << 30)
#define PCI_BAR0_FLAGS 0x4U
#define PCI_BAR0_LOW_MASK 0xfffc0000U
#define PCI_CONFIG_DWORDS (sizeof(((struct pci_fe *)0)->config_space) / 4U)

#define SRDMA_HEALTH_MAC0_ALIVE (1U << 0)
#define SRDMA_HEALTH_MAC1_ALIVE (1U << 1)

#define TLP_FMT_3DW_NODATA 0x0U
#define TLP_FMT_4DW_NODATA 0x1U
#define TLP_FMT_3DW_W_DATA 0x2U
#define TLP_FMT_4DW_W_DATA 0x3U
#define TLP_REQ_TYPE_MEMORY 0x0U
#define TLP_REQ_TYPE_CONFIG0 0x4U
#define TLP_REQ_TYPE_CONFIG1 0x5U
#define TLP_TYPE_COMPLETION 0x0aU
#define TLP_CPL_STATUS_SC 0x0U
#define TLP_CPL_STATUS_UR 0x1U
#define TLP_MAX_DATA_DWORDS 16U

#define GET_TLP_FMT(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 29), ((const uint32_t *)(h))[0])
#define GET_TLP_TYPE(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(28, 24), ((const uint32_t *)(h))[0])
#define GET_TLP_TAG9(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(23, 23), ((const uint32_t *)(h))[0])
#define GET_TLP_TAG8(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(19, 19), ((const uint32_t *)(h))[0])
#define GET_TLP_LENGTH(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(9, 0), ((const uint32_t *)(h))[0])
#define GET_TLP_REQUESTER(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 16), ((const uint32_t *)(h))[1])
#define GET_TLP_TAG(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(15, 8), ((const uint32_t *)(h))[1])
#define GET_TLP_LAST_BE(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(7, 4), ((const uint32_t *)(h))[1])
#define GET_TLP_FIRST_BE(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(3, 0), ((const uint32_t *)(h))[1])
#define GET_TLP_BUS(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 24), ((const uint32_t *)(h))[2])
#define GET_TLP_DEVICE(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(23, 19), ((const uint32_t *)(h))[2])
#define GET_TLP_FUNCTION(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(18, 16), ((const uint32_t *)(h))[2])
#define GET_TLP_EXT_REG(h) DOCA_BE32_GET(DOCA_BE32_GENMASK(11, 2), ((const uint32_t *)(h))[2])

enum pci_fe_tlp_kind {
    PCI_FE_TLP_INVALID = 0,
    PCI_FE_TLP_MRD,
    PCI_FE_TLP_MWR,
    PCI_FE_TLP_CFGRD0,
    PCI_FE_TLP_CFGWR0,
    PCI_FE_TLP_CFGRD1,
    PCI_FE_TLP_CFGWR1,
};

static uint32_t byte_enable_mask(uint8_t be)
{
    uint32_t mask = 0;

    for (unsigned int i = 0; i < 4; i++) {
        if ((be & (1U << i)) != 0)
            mask |= 0xffU << (i * 8U);
    }
    return mask;
}

static unsigned int enabled_bytes(uint8_t be)
{
    unsigned int count = 0;

    for (unsigned int i = 0; i < 4; i++)
        count += (be >> i) & 1U;
    return count;
}

static unsigned int first_enabled_byte(uint8_t be)
{
    for (unsigned int i = 0; i < 4; i++) {
        if ((be & (1U << i)) != 0)
            return i;
    }
    return 0;
}

static uint32_t load_config_dword(const struct pci_fe *fe, uint32_t reg)
{
    uint32_t value = 0;

    if (reg >= PCI_CONFIG_DWORDS)
        return 0;
    memcpy(&value, &fe->config_space[reg * 4U], sizeof(value));
    if (reg == PCI_BAR0_LOW_OFFSET / 4U && fe->bar_probe_low)
        return PCI_BAR0_LOW_MASK | PCI_BAR0_FLAGS;
    if (reg == PCI_BAR0_HIGH_OFFSET / 4U && fe->bar_probe_high)
        return 0xffffffffU;
    return value;
}

static void store_config_dword(struct pci_fe *fe, uint32_t reg,
                               uint32_t value)
{
    if (reg < PCI_CONFIG_DWORDS)
        memcpy(&fe->config_space[reg * 4U], &value, sizeof(value));
}

static void update_bar0_base(struct pci_fe *fe)
{
    uint32_t low = load_config_dword(fe, PCI_BAR0_LOW_OFFSET / 4U);
    uint32_t high = load_config_dword(fe, PCI_BAR0_HIGH_OFFSET / 4U);

    if (fe->bar_probe_low || fe->bar_probe_high)
        return;
    fe->bar0_base = ((uint64_t)high << 32) | (low & PCI_BAR0_LOW_MASK);
}

static void init_config_space(struct pci_fe *fe)
{
    uint32_t value;

    memset(fe->config_space, 0, sizeof(fe->config_space));
    value = VFIO_ADMINQ_PCI_VENDOR_ID | (VFIO_ADMINQ_PCI_DEVICE_ID << 16);
    store_config_dword(fe, 0, value);
    store_config_dword(fe, PCI_COMMAND_OFFSET / 4U, 1U << 20);
    value = (VFIO_ADMINQ_PCI_CLASS_CODE << 8) | VFIO_ADMINQ_PCI_REVISION;
    store_config_dword(fe, 2, value);
    store_config_dword(fe, PCI_BAR0_LOW_OFFSET / 4U, PCI_BAR0_FLAGS);
    store_config_dword(fe, PCI_BAR0_HIGH_OFFSET / 4U, 0);
    value = VFIO_ADMINQ_PCI_VENDOR_ID | (VFIO_ADMINQ_PCI_DEVICE_ID << 16);
    store_config_dword(fe, PCI_SUBSYSTEM_OFFSET / 4U, value);
    store_config_dword(fe, PCI_CAP_PTR_OFFSET / 4U, PCI_MSIX_CAP_OFFSET);
    value = PCI_MSIX_CAP_ID | (PCI_EXP_CAP_OFFSET << 8) |
            ((VFIO_ADMINQ_NUM_MSIX - 1U) << 16);
    store_config_dword(fe, PCI_MSIX_CAP_OFFSET / 4U, value);
    store_config_dword(fe, PCI_MSIX_CAP_OFFSET / 4U + 1U,
                       VFIO_ADMINQ_MSIX_TABLE_OFFSET | VFIO_ADMINQ_BAR0_ID);
    store_config_dword(fe, PCI_MSIX_CAP_OFFSET / 4U + 2U,
                       VFIO_ADMINQ_MSIX_PBA_OFFSET | VFIO_ADMINQ_BAR0_ID);
    store_config_dword(fe, PCI_EXP_CAP_OFFSET / 4U,
                       PCI_EXP_CAP_ID | (2U << 16));
    store_config_dword(fe, PCI_EXP_DEVCAP_OFFSET / 4U, PCI_EXP_DEVCAP_FLR);

    fe->bar0_base = 0;
    fe->bar_probe_low = false;
    fe->bar_probe_high = false;
    fe->memory_enable = false;
    fe->bus_master_enable = false;
    fe->bdf = 0;
}

static void reset_bar_state(struct pci_fe *fe)
{
    fe->adminq_tx_addr = 0;
    fe->adminq_rx_addr = 0;
    fe->asyncq_addr = 0;
    fe->adminq_depth = 0;
    fe->asyncq_depth = 0;
    fe->init_done = false;
    fe->pending_action = PCI_FE_ACTION_NONE;
}

static uint32_t bar0_read32(const struct pci_fe *fe, uint32_t offset)
{
    uint32_t value = 0;

    switch (offset) {
    case SRDMA_BFA_PCI_VERSION:
        return 1;
    case SRDMA_BFA_PCI_DEV_READY:
        return fe->ready ? 1U : 0U;
    case SRDMA_BFA_MAX_QP_NUM:
        return 128;
    case SRDMA_BFA_PCI_MAX_VECTORS:
        return VFIO_ADMINQ_NUM_MSIX;
    case SRDMA_BFA_PCI_DEV_MACADDR:
        return (uint32_t)fe->mac[5] | ((uint32_t)fe->mac[4] << 8) |
               ((uint32_t)fe->mac[3] << 16) | ((uint32_t)fe->mac[2] << 24);
    case SRDMA_BFA_PCI_DEV_MACADDR + 4:
        return (uint32_t)fe->mac[1] | ((uint32_t)fe->mac[0] << 8);
    case SRDMA_BFA_PCI_DEV_CTRL:
        return 0;
    case SRDMA_BFA_PCI_DEV_STATUS:
        return fe->init_done ? SRDMA_BFA_PCI_DEV_STATUS_INIT_DONE : 0;
    case SRDMA_BFA_PCI_DEV_ADMIN_TXQ_BASEADDR_LO:
        return (uint32_t)fe->adminq_tx_addr;
    case SRDMA_BFA_PCI_DEV_ADMIN_TXQ_BASEADDR_HI:
        return (uint32_t)(fe->adminq_tx_addr >> 32);
    case SRDMA_BFA_PCI_DEV_ADMIN_RXQ_BASEADDR_LO:
        return (uint32_t)fe->adminq_rx_addr;
    case SRDMA_BFA_PCI_DEV_ADMIN_RXQ_BASEADDR_HI:
        return (uint32_t)(fe->adminq_rx_addr >> 32);
    case SRDMA_BFA_PCI_DEV_ASYNCQ_BASEADDR_LO:
        return (uint32_t)fe->asyncq_addr;
    case SRDMA_BFA_PCI_DEV_ASYNCQ_BASEADDR_HI:
        return (uint32_t)(fe->asyncq_addr >> 32);
    case SRDMA_BFA_PCI_DEV_ADMINQ_DEPTH:
        return fe->adminq_depth;
    case SRDMA_BFA_PCI_DEV_ASYNCQ_DEPTH:
        return fe->asyncq_depth;
    case SRDMA_BFA_PCI_DEV_DIAG:
        return fe->heartbeat;
    case SRDMA_BFA_PCI_DEV_DIAG + 4:
        return 0x00010000U;
    case SRDMA_BFA_PCI_DEV_DIAG + 8:
        return 1U;
    case SRDMA_BFA_PCI_DEV_DIAG + 12:
        return SRDMA_HEALTH_MAC0_ALIVE | SRDMA_HEALTH_MAC1_ALIVE;
    default:
        if (offset >= SRDMA_BFA_PCI_DEV_DIAG &&
            offset < SRDMA_BFA_PCI_DEV_DIAG + SRDMA_BFA_PCI_DEV_DIAG_SIZE)
            return 0;
        return value;
    }
}

static void merge_u64_low(uint64_t *target, uint32_t value)
{
    *target = (*target & 0xffffffff00000000ULL) | value;
}

static void merge_u64_high(uint64_t *target, uint32_t value)
{
    *target = (*target & 0x00000000ffffffffULL) | ((uint64_t)value << 32);
}

static void request_stop(struct pci_fe *fe)
{
    if (fe->state == PCI_FE_STARTED || fe->state == PCI_FE_STARTING)
        fe->pending_action = PCI_FE_ACTION_STOP;
}

static void bar0_write32(struct pci_fe *fe, uint32_t offset,
                         uint32_t value, uint32_t mask)
{
    uint32_t old = bar0_read32(fe, offset);
    uint32_t merged = (old & ~mask) | (value & mask);

    switch (offset) {
    case SRDMA_BFA_PCI_DEV_CTRL:
        if ((merged & SRDMA_BFA_PCI_DEV_CTRL_CMD_RESET) != 0)
            request_stop(fe);
        if ((merged & SRDMA_BFA_PCI_DEV_CTRL_CMD_INIT_DONE) != 0 &&
            fe->state == PCI_FE_PRESENT_STOPPED)
            fe->pending_action = PCI_FE_ACTION_START;
        break;
    case SRDMA_BFA_PCI_DEV_ADMIN_TXQ_BASEADDR_LO:
        merge_u64_low(&fe->adminq_tx_addr, merged);
        break;
    case SRDMA_BFA_PCI_DEV_ADMIN_TXQ_BASEADDR_HI:
        merge_u64_high(&fe->adminq_tx_addr, merged);
        break;
    case SRDMA_BFA_PCI_DEV_ADMIN_RXQ_BASEADDR_LO:
        merge_u64_low(&fe->adminq_rx_addr, merged);
        break;
    case SRDMA_BFA_PCI_DEV_ADMIN_RXQ_BASEADDR_HI:
        merge_u64_high(&fe->adminq_rx_addr, merged);
        break;
    case SRDMA_BFA_PCI_DEV_ASYNCQ_BASEADDR_LO:
        merge_u64_low(&fe->asyncq_addr, merged);
        break;
    case SRDMA_BFA_PCI_DEV_ASYNCQ_BASEADDR_HI:
        merge_u64_high(&fe->asyncq_addr, merged);
        break;
    case SRDMA_BFA_PCI_DEV_ADMINQ_DEPTH:
        fe->adminq_depth = merged;
        break;
    case SRDMA_BFA_PCI_DEV_ASYNCQ_DEPTH:
        fe->asyncq_depth = merged;
        break;
    default:
        break;
    }
}

static void handle_config_write(struct pci_fe *fe, uint32_t reg,
                                uint32_t value, uint8_t first_be)
{
    uint32_t mask = byte_enable_mask(first_be);
    uint32_t old = load_config_dword(fe, reg);
    uint32_t merged = (old & ~mask) | (value & mask);

    switch (reg * 4U) {
    case PCI_COMMAND_OFFSET: {
        uint16_t old_command = (uint16_t)old;
        uint16_t command = (uint16_t)merged;

        command &= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
        merged = (old & 0xffff0000U) | command;
        fe->memory_enable = (command & PCI_COMMAND_MEMORY) != 0;
        fe->bus_master_enable = (command & PCI_COMMAND_MASTER) != 0;
        if ((old_command & PCI_COMMAND_MASTER) != 0 &&
            (command & PCI_COMMAND_MASTER) == 0)
            request_stop(fe);
        store_config_dword(fe, reg, merged);
        break;
    }
    case PCI_BAR0_LOW_OFFSET:
        if (value == 0xffffffffU && mask == 0xffffffffU) {
            fe->bar_probe_low = true;
        } else {
            fe->bar_probe_low = false;
            store_config_dword(fe, reg,
                               (merged & PCI_BAR0_LOW_MASK) | PCI_BAR0_FLAGS);
            update_bar0_base(fe);
        }
        break;
    case PCI_BAR0_HIGH_OFFSET:
        if (value == 0xffffffffU && mask == 0xffffffffU) {
            fe->bar_probe_high = true;
        } else {
            fe->bar_probe_high = false;
            store_config_dword(fe, reg, merged);
            update_bar0_base(fe);
        }
        break;
    case PCI_MSIX_CAP_OFFSET: {
        uint32_t writable = PCI_MSIX_MSGCTRL_ENABLE | PCI_MSIX_MSGCTRL_MASKALL;
        store_config_dword(fe, reg, (old & ~writable) | (merged & writable));
        break;
    }
    case PCI_EXP_DEVCTL_OFFSET: {
        if ((merged & PCI_EXP_DEVCTL_FLR) != 0) {
            bool needs_stop = fe->state == PCI_FE_STARTED ||
                              fe->state == PCI_FE_STARTING;

            reset_bar_state(fe);
            if (needs_stop)
                fe->pending_action = PCI_FE_ACTION_STOP;
        }
        store_config_dword(fe, reg, merged & ~PCI_EXP_DEVCTL_FLR);
        break;
    }
    default:
        /* All other fields are read-only in the minimal model. */
        break;
    }
}

static enum pci_fe_tlp_kind get_tlp_kind(const void *header)
{
    uint8_t fmt = GET_TLP_FMT(header);
    uint8_t type = GET_TLP_TYPE(header);
    bool has_data = (fmt & 0x2U) != 0;

    if (type == TLP_REQ_TYPE_MEMORY)
        return has_data ? PCI_FE_TLP_MWR : PCI_FE_TLP_MRD;
    if (type == TLP_REQ_TYPE_CONFIG0)
        return has_data ? PCI_FE_TLP_CFGWR0 : PCI_FE_TLP_CFGRD0;
    if (type == TLP_REQ_TYPE_CONFIG1)
        return has_data ? PCI_FE_TLP_CFGWR1 : PCI_FE_TLP_CFGRD1;
    return PCI_FE_TLP_INVALID;
}

static uint64_t tlp_memory_address(const void *header)
{
    const uint32_t *dw = header;
    uint8_t fmt = GET_TLP_FMT(header);

    if (fmt == TLP_FMT_4DW_NODATA || fmt == TLP_FMT_4DW_W_DATA)
        return ((uint64_t)be32toh(dw[2]) << 32) |
               (be32toh(dw[3]) & ~3U);
    return be32toh(dw[2]) & ~3U;
}

static void set_completion_header(struct doca_devemu_pci_tlp_channel_req *req,
                                  struct pci_fe *fe, const void *request_header,
                                  uint8_t fmt, uint16_t length,
                                  uint8_t status, uint16_t byte_count,
                                  uint8_t lower_addr)
{
    uint32_t *dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(req);

    memset(dw, 0, 3U * sizeof(*dw));
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 29), fmt, &dw[0]);
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(28, 24), TLP_TYPE_COMPLETION, &dw[0]);
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(23, 23), GET_TLP_TAG9(request_header), &dw[0]);
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(19, 19), GET_TLP_TAG8(request_header), &dw[0]);
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(9, 0), length, &dw[0]);
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), fe->bdf, &dw[1]);
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 13), status, &dw[1]);
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(11, 0), byte_count, &dw[1]);
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), GET_TLP_REQUESTER(request_header), &dw[2]);
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 8), GET_TLP_TAG(request_header), &dw[2]);
    DOCA_BE32P_SET(DOCA_BE32_GENMASK(6, 0), lower_addr, &dw[2]);
}

static void complete_ur(struct doca_devemu_pci_tlp_channel_req *req,
                        struct pci_fe *fe, const void *header)
{
    set_completion_header(req, fe, header, 0, 0, TLP_CPL_STATUS_UR, 0, 0);
    doca_devemu_pci_tlp_channel_req_complete_tlp(req, 1, fe->tlp_dev);
}

static bool config_target_valid(struct pci_fe *fe, const void *header)
{
    uint8_t bus = GET_TLP_BUS(header);
    uint8_t device = GET_TLP_DEVICE(header);
    uint8_t function = GET_TLP_FUNCTION(header);

    if (device != 0 || function != 0)
        return false;
    fe->bdf = ((uint16_t)bus << 8) | ((uint16_t)device << 3) | function;
    return true;
}

static bool config_capability(uint32_t reg, uint16_t *cap_id,
                              uint8_t *is_pcie_cap)
{
    uint32_t offset = reg * 4U;

    *is_pcie_cap = 0;
    if (offset >= PCI_MSIX_CAP_OFFSET &&
        offset < PCI_MSIX_CAP_OFFSET + 12U) {
        *cap_id = PCI_MSIX_CAP_ID;
        return true;
    }
    if (offset >= PCI_EXP_CAP_OFFSET &&
        offset < PCI_EXP_CAP_OFFSET + PCI_EXP_CAP_LENGTH) {
        *cap_id = PCI_EXP_CAP_ID;
        return true;
    }
    return false;
}

static void handle_config_tlp(struct doca_devemu_pci_tlp_channel_req *req,
                              struct pci_fe *fe, const void *header,
                              enum pci_fe_tlp_kind kind)
{
    uint32_t reg = GET_TLP_EXT_REG(header);
    uint16_t cap_id = 0;
    uint8_t is_pcie_cap = 0;
    uint8_t is_cap_id_valid;

    if (!config_target_valid(fe, header) ||
        (kind != PCI_FE_TLP_CFGRD0 && kind != PCI_FE_TLP_CFGWR0)) {
        complete_ur(req, fe, header);
        return;
    }

    is_cap_id_valid = config_capability(reg, &cap_id, &is_pcie_cap);
    if (kind == PCI_FE_TLP_CFGRD0) {
        uint32_t value = load_config_dword(fe, reg);
        void *data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(req);

        memcpy(data, &value, sizeof(value));
        /*
         * Unlike a memory-read completion, a configuration-read completion
         * must not copy the configuration register offset into Lower Address.
         * A non-zero value makes the Root Port classify the completion as
         * unexpected (and may trigger DPC containment).
         */
        set_completion_header(req, fe, header, 2, 1, TLP_CPL_STATUS_SC,
                              4, 0);
        doca_devemu_pci_tlp_channel_req_complete_config_read(req,
                                                              fe->tlp_dev,
                                                              is_cap_id_valid,
                                                              cap_id,
                                                              is_pcie_cap);
    } else {
        const uint32_t *data = doca_devemu_pci_tlp_channel_req_get_tlp_data(req);

        if (data == NULL) {
            complete_ur(req, fe, header);
            return;
        }
        handle_config_write(fe, reg, data[0], GET_TLP_FIRST_BE(header));
        set_completion_header(req, fe, header, 0, 0, TLP_CPL_STATUS_SC, 4, 0);
        doca_devemu_pci_tlp_channel_req_complete_config_write(req,
                                                               fe->tlp_dev,
                                                               is_cap_id_valid,
                                                               cap_id,
                                                               is_pcie_cap);
    }
}

enum pci_fe_memory_bar {
    PCI_FE_MEMORY_NONE = 0,
    PCI_FE_MEMORY_BAR0,
};

static enum pci_fe_memory_bar memory_request_valid(
    struct pci_fe *fe, uint64_t address, uint32_t length_bytes,
    uint32_t *bar_offset)
{
    uint64_t end;

    if (!fe->memory_enable || length_bytes == 0)
        return PCI_FE_MEMORY_NONE;
    if (__builtin_add_overflow(address, (uint64_t)length_bytes, &end))
        return PCI_FE_MEMORY_NONE;
    if (fe->bar0_base != 0 && address >= fe->bar0_base &&
        end <= fe->bar0_base + VFIO_ADMINQ_HOST_BAR0_SIZE) {
        *bar_offset = (uint32_t)(address - fe->bar0_base);
        return PCI_FE_MEMORY_BAR0;
    }
    return PCI_FE_MEMORY_NONE;
}

static void handle_memory_read(struct doca_devemu_pci_tlp_channel_req *req,
                               struct pci_fe *fe, const void *header)
{
    uint32_t length = GET_TLP_LENGTH(header);
    uint32_t offset;
    uint64_t address = tlp_memory_address(header);
    uint8_t first_be = GET_TLP_FIRST_BE(header);
    uint8_t last_be = GET_TLP_LAST_BE(header);
    uint32_t *data;
    unsigned int byte_count = 0;
    enum pci_fe_memory_bar bar;

    bar = memory_request_valid(fe, address, length * 4U, &offset);
    if (length == 0 || length > TLP_MAX_DATA_DWORDS ||
        bar == PCI_FE_MEMORY_NONE) {
        complete_ur(req, fe, header);
        return;
    }

    data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(req);
    for (uint32_t i = 0; i < length; i++) {
        data[i] = bar == PCI_FE_MEMORY_BAR0 &&
                          offset + i * 4U < VFIO_ADMINQ_BAR0_CFG_SIZE ?
                      bar0_read32(fe, (offset + i * 4U) & ~3U) : 0;
        if (length == 1)
            byte_count += enabled_bytes(first_be);
        else if (i == 0)
            byte_count += enabled_bytes(first_be);
        else if (i == length - 1)
            byte_count += enabled_bytes(last_be);
        else
            byte_count += 4;
    }
    if (byte_count == 0)
        byte_count = length * 4U;
    set_completion_header(req, fe, header, 2, (uint16_t)length,
                          TLP_CPL_STATUS_SC, (uint16_t)byte_count,
                          (uint8_t)((address + first_enabled_byte(first_be)) & 0x7fU));
    doca_devemu_pci_tlp_channel_req_complete_tlp(req, 1, fe->tlp_dev);
}

static void handle_memory_write(struct doca_devemu_pci_tlp_channel_req *req,
                                struct pci_fe *fe, const void *header)
{
    uint32_t length = GET_TLP_LENGTH(header);
    uint32_t offset;
    uint64_t address = tlp_memory_address(header);
    uint8_t first_be = GET_TLP_FIRST_BE(header);
    uint8_t last_be = GET_TLP_LAST_BE(header);
    const uint32_t *data = doca_devemu_pci_tlp_channel_req_get_tlp_data(req);
    enum pci_fe_memory_bar bar;

    bar = memory_request_valid(fe, address, length * 4U, &offset);
    if (length == 0 || length > TLP_MAX_DATA_DWORDS || data == NULL ||
        bar == PCI_FE_MEMORY_NONE) {
        doca_devemu_pci_tlp_channel_req_complete_tlp(req, 0, fe->tlp_dev);
        return;
    }
    for (uint32_t i = 0; i < length; i++) {
        uint8_t be = 0xfU;

        if (length == 1)
            be = first_be;
        else if (i == 0)
            be = first_be;
        else if (i == length - 1)
            be = last_be;
        if (be == 0)
            be = 0xfU;
        if (offset + i * 4U < VFIO_ADMINQ_BAR0_CFG_SIZE) {
            bar0_write32(fe, (offset + i * 4U) & ~3U,
                         data[i], byte_enable_mask(be));
        }
    }
    doca_devemu_pci_tlp_channel_req_complete_tlp(req, 0, fe->tlp_dev);
}

static void handle_tlp_request(struct doca_devemu_pci_tlp_channel_req *req,
                               struct pci_fe *fe)
{
    const void *header = doca_devemu_pci_tlp_channel_req_get_tlp_header(req);
    enum pci_fe_tlp_kind kind;

    if (header == NULL || fe->tlp_dev == NULL) {
        doca_devemu_pci_tlp_channel_req_complete_tlp(req, 0, NULL);
        return;
    }
    kind = get_tlp_kind(header);
    switch (kind) {
    case PCI_FE_TLP_CFGRD0:
    case PCI_FE_TLP_CFGWR0:
    case PCI_FE_TLP_CFGRD1:
    case PCI_FE_TLP_CFGWR1:
        handle_config_tlp(req, fe, header, kind);
        break;
    case PCI_FE_TLP_MRD:
        handle_memory_read(req, fe, header);
        break;
    case PCI_FE_TLP_MWR:
        handle_memory_write(req, fe, header);
        break;
    default:
        complete_ur(req, fe, header);
        break;
    }
}

static void handle_pci_event(struct doca_devemu_pci_tlp_channel_req *req,
                             struct pci_fe *fe)
{
    enum doca_devemu_pci_tlp_channel_req_pci_event_opmode mode =
        doca_devemu_pci_tlp_channel_req_get_pci_event_opmode(req);

    if (mode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_PCI_EVENT_OPMODE_PERST_ASSERT) {
        bool needs_stop = fe->state == PCI_FE_STARTED ||
                          fe->state == PCI_FE_STARTING;

        init_config_space(fe);
        reset_bar_state(fe);
        if (needs_stop)
            fe->pending_action = PCI_FE_ACTION_STOP;
    }
    doca_devemu_pci_tlp_channel_req_complete_pci_event(req);
}

static void tlp_request_cb(struct doca_devemu_pci_tlp_channel *channel,
                           struct doca_devemu_pci_tlp_channel_req *req,
                           void *req_user_data)
{
    union doca_data user_data;
    struct pci_fe *fe;
    enum doca_devemu_pci_tlp_channel_req_opcode opcode;

    (void)req_user_data;
    if (doca_ctx_get_user_data(doca_devemu_pci_tlp_channel_as_ctx(channel),
                               &user_data) != DOCA_SUCCESS)
        return;
    fe = user_data.ptr;
    opcode = doca_devemu_pci_tlp_channel_req_get_opcode(req);
    if (opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_PCI_EVENT)
        handle_pci_event(req, fe);
    else if (opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_TLP)
        handle_tlp_request(req, fe);
    else
        DOCA_LOG_WARN("unsupported TLP channel opcode %d", opcode);
}

static doca_error_t find_tlp_device(const char *pci_addr,
                                    struct doca_dev **dev)
{
    struct doca_devinfo **list = NULL;
    uint32_t count = 0;
    doca_error_t result;

    *dev = NULL;
    result = doca_devinfo_create_list(&list, &count);
    if (result != DOCA_SUCCESS)
        return result;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t equal = 0;
        uint16_t max_types = 0;

        if (doca_devinfo_is_equal_pci_addr(list[i], pci_addr, &equal) != DOCA_SUCCESS || !equal)
            continue;
        if (doca_devemu_pci_tlp_cap_get_max_types(list[i], &max_types) != DOCA_SUCCESS || max_types == 0)
            continue;
        result = doca_dev_open(list[i], dev);
        doca_devinfo_destroy_list(list);
        return result;
    }
    doca_devinfo_destroy_list(list);
    return DOCA_ERROR_NOT_FOUND;
}

static doca_error_t init_pci_type(struct pci_fe *fe)
{
    doca_error_t result;

    result = doca_devemu_pci_tlp_type_create(VFIO_ADMINQ_PCI_TYPE_NAME,
                                              &fe->pci_type);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_type_set_dev(fe->pci_type, fe->dev);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_type_set_num_msix(fe->pci_type,
                                                VFIO_ADMINQ_NUM_MSIX);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("set_num_msix failed: %s", doca_error_get_descr(result));
        return result;
    }
    result = doca_devemu_pci_type_set_num_db(fe->pci_type,
                                              VFIO_ADMINQ_DB_COUNT);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("set_num_db failed: %s", doca_error_get_descr(result));
        return result;
    }
    result = doca_devemu_pci_type_set_memory_bar_conf(
        fe->pci_type, VFIO_ADMINQ_DOCA_BAR0_ID, VFIO_ADMINQ_DOCA_BAR0_LOG_SIZE,
        DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT, 0);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("BAR0 config failed: %s", doca_error_get_descr(result));
        return result;
    }
    result = doca_devemu_pci_type_set_memory_bar_conf(
        fe->pci_type, 1, 0, DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT, 0);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("BAR1 disable failed: %s", doca_error_get_descr(result));
        return result;
    }
    result = doca_devemu_pci_tlp_type_set_bar_transaction_region_conf(
        fe->pci_type, VFIO_ADMINQ_DOCA_BAR0_ID, VFIO_ADMINQ_BAR0_CFG_OFFSET,
        VFIO_ADMINQ_BAR0_CFG_SIZE);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("BAR0 transaction region failed: %s", doca_error_get_descr(result));
        return result;
    }
    result = doca_devemu_pci_type_set_bar_db_region_by_offset_conf(
        fe->pci_type, VFIO_ADMINQ_DOCA_BAR0_ID,
        VFIO_ADMINQ_DB_REGION_OFFSET, VFIO_ADMINQ_DB_REGION_SIZE,
        VFIO_ADMINQ_DB_LOG_SIZE, VFIO_ADMINQ_DB_STRIDE_LOG_SIZE);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("UAR doorbell region failed: %s",
                     doca_error_get_descr(result));
        return result;
    }
    result = doca_devemu_pci_type_set_bar_msix_table_region_conf(
        fe->pci_type, VFIO_ADMINQ_DOCA_BAR0_ID, VFIO_ADMINQ_MSIX_TABLE_OFFSET,
        VFIO_ADMINQ_MSIX_TABLE_SIZE);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("MSI-X table region failed: %s", doca_error_get_descr(result));
        return result;
    }
    result = doca_devemu_pci_type_set_bar_msix_pba_region_conf(
        fe->pci_type, VFIO_ADMINQ_DOCA_BAR0_ID, VFIO_ADMINQ_MSIX_PBA_OFFSET,
        VFIO_ADMINQ_MSIX_PBA_SIZE);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("MSI-X PBA region failed: %s", doca_error_get_descr(result));
        return result;
    }
    result = doca_devemu_pci_tlp_type_set_pci_cap_conf(
        fe->pci_type, PCI_MSIX_CAP_ID, PCI_MSIX_CAP_OFFSET, 12);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("MSI-X capability config failed: %s", doca_error_get_descr(result));
        return result;
    }
    result = doca_devemu_pci_tlp_type_set_pci_cap_conf(
        fe->pci_type, PCI_EXP_CAP_ID, PCI_EXP_CAP_OFFSET,
        PCI_EXP_CAP_LENGTH);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("PCI Express capability config failed: %s",
                     doca_error_get_descr(result));
        return result;
    }
    result = doca_devemu_pci_type_start(fe->pci_type);
    if (result != DOCA_SUCCESS)
        DOCA_LOG_ERR("PCI type start failed: %s", doca_error_get_descr(result));
    if (result == DOCA_SUCCESS) {
        struct doca_devemu_pci_bar_info **bars = NULL;
        uint32_t count = 0;
        bool found_bar0 = false;

        fe->pci_type_started = true;
        result = doca_devemu_pci_type_create_bar_info_list(fe->pci_type,
                                                           &bars, &count);
        if (result != DOCA_SUCCESS)
            return result;
        for (uint32_t i = 0; i < count; i++) {
            uint8_t id = UINT8_MAX;
            uint8_t log_size = 0;
            (void)doca_devemu_pci_bar_info_get_bar_id(bars[i], &id);
            (void)doca_devemu_pci_bar_info_get_log_sz(bars[i], &log_size);
            found_bar0 |= id == VFIO_ADMINQ_DOCA_BAR0_ID &&
                          log_size == VFIO_ADMINQ_DOCA_BAR0_LOG_SIZE;
        }
        (void)doca_devemu_pci_type_destroy_bar_info_list(bars);
        if (!found_bar0)
            return DOCA_ERROR_BAD_STATE;
    }
    return result;
}

static doca_error_t init_tlp_channel(struct pci_fe *fe)
{
    union doca_data user_data = {.ptr = fe};
    uint8_t num_dsp = 0;
    doca_error_t result;

    result = doca_pe_create(&fe->pe);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_tlp_channel_create(fe->dev, &fe->tlp_channel);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_tlp_channel_set_primary(fe->tlp_channel, 1);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_tlp_channel_set_shm_dir_path(fe->tlp_channel,
                                                          "/dev/shm");
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_tlp_channel_set_req_user_data_size(fe->tlp_channel,
                                                                128);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_tlp_channel_event_req_register(fe->tlp_channel,
                                                            tlp_request_cb);
    if (result != DOCA_SUCCESS)
        return result;
    fe->channel_ctx = doca_devemu_pci_tlp_channel_as_ctx(fe->tlp_channel);
    result = doca_pe_connect_ctx(fe->pe, fe->channel_ctx);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_ctx_set_user_data(fe->channel_ctx, user_data);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_ctx_start(fe->channel_ctx);
    if (result != DOCA_SUCCESS)
        return result;
    fe->channel_started = true;
    result = doca_devemu_pci_tlp_channel_get_num_dsp(fe->tlp_channel, &num_dsp);
    if (result != DOCA_SUCCESS)
        return result;
    if (num_dsp != 1) {
        DOCA_LOG_ERR("vfio-adminq requires exactly one TLP downstream port; got %u", num_dsp);
        return DOCA_ERROR_NOT_SUPPORTED;
    }
    return DOCA_SUCCESS;
}

static void progress_cb(void *opaque)
{
    struct pci_fe *fe = opaque;

    if (fe->pe != NULL)
        while (doca_pe_progress(fe->pe) != 0) {
        }
}

static void destroy_endpoint(struct pci_fe *fe)
{
    if (fe->tlp_dev != NULL) {
        if (fe->tlp_dev_started)
            (void)doca_devemu_pci_tlp_dev_stop(fe->tlp_dev);
        (void)doca_devemu_pci_tlp_dev_destroy(fe->tlp_dev);
        fe->tlp_dev = NULL;
        fe->tlp_dev_started = false;
    }
    if (fe->rep != NULL) {
        (void)doca_devemu_pci_type_destroy_rep(fe->rep);
        fe->rep = NULL;
    }
    fe->ready = false;
    fe->vhca_id = 0;
    init_config_space(fe);
    reset_bar_state(fe);
}

static doca_error_t create_endpoint(struct pci_fe *fe)
{
    const struct doca_devinfo_rep *rep_info;
    struct doca_devemu_pci_ep *ep;
    doca_error_t result;

    result = doca_devemu_pci_type_create_rep(fe->pci_type, &fe->rep);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_tlp_dev_create(fe->pci_type, fe->rep,
                                            &fe->tlp_dev);
    if (result != DOCA_SUCCESS)
        return result;
    ep = doca_devemu_pci_tlp_dev_as_ep(fe->tlp_dev);
    result = doca_devemu_pci_ep_set_num_db(ep, VFIO_ADMINQ_DB_COUNT);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_ep_set_num_msix(ep, VFIO_ADMINQ_NUM_MSIX);
    if (result != DOCA_SUCCESS)
        return result;
    result = doca_devemu_pci_tlp_dev_start(fe->tlp_dev);
    if (result != DOCA_SUCCESS)
        return result;
    fe->tlp_dev_started = true;
    rep_info = doca_dev_rep_as_devinfo(fe->rep);
    result = doca_devinfo_rep_get_vhca_id(rep_info, &fe->vhca_id);
    return result;
}

doca_error_t pci_fe_init(struct pci_fe *fe, const char *pci_addr,
                         struct gemini_server *gemini,
                         const uint8_t mac[6])
{
    doca_error_t result;

    if (fe == NULL || pci_addr == NULL || gemini == NULL || mac == NULL)
        return DOCA_ERROR_INVALID_VALUE;
    memset(fe, 0, sizeof(*fe));
    fe->gemini = gemini;
    fe->state = PCI_FE_ABSENT;
    memcpy(fe->mac, mac, sizeof(fe->mac));
    init_config_space(fe);
    reset_bar_state(fe);

    result = find_tlp_device(pci_addr, &fe->dev);
    if (result != DOCA_SUCCESS)
        goto fail;
    result = init_pci_type(fe);
    if (result != DOCA_SUCCESS)
        goto fail;
    result = init_tlp_channel(fe);
    if (result != DOCA_SUCCESS)
        goto fail;
    return DOCA_SUCCESS;

fail:
    pci_fe_cleanup(fe);
    return result;
}

void pci_fe_cleanup(struct pci_fe *fe)
{
    enum doca_ctx_states state;

    if (fe == NULL)
        return;
    if (fe->state != PCI_FE_ABSENT)
        (void)pci_fe_unplug(fe, true);
    destroy_endpoint(fe);
    if (fe->channel_ctx != NULL && fe->channel_started) {
        doca_error_t result = doca_ctx_stop(fe->channel_ctx);

        if (result == DOCA_ERROR_IN_PROGRESS) {
            for (unsigned int i = 0; i < 10000; i++) {
                while (fe->pe != NULL && doca_pe_progress(fe->pe) != 0) {
                }
                if (doca_ctx_get_state(fe->channel_ctx, &state) == DOCA_SUCCESS &&
                    state == DOCA_CTX_STATE_IDLE)
                    break;
            }
        }
        fe->channel_started = false;
    }
    if (fe->tlp_channel != NULL) {
        (void)doca_devemu_pci_tlp_channel_destroy(fe->tlp_channel);
        fe->tlp_channel = NULL;
        fe->channel_ctx = NULL;
    }
    if (fe->pe != NULL) {
        (void)doca_pe_destroy(fe->pe);
        fe->pe = NULL;
    }
    if (fe->pci_type != NULL) {
        if (fe->pci_type_started)
            (void)doca_devemu_pci_type_stop(fe->pci_type);
        (void)doca_devemu_pci_type_destroy(fe->pci_type);
        fe->pci_type = NULL;
        fe->pci_type_started = false;
    }
    if (fe->dev != NULL) {
        (void)doca_dev_close(fe->dev);
        fe->dev = NULL;
    }
    fe->state = PCI_FE_ABSENT;
}

void pci_fe_progress(struct pci_fe *fe)
{
    if (fe != NULL) {
        struct timespec now;
        uint64_t now_ns;

        if (fe->state == PCI_FE_STARTED &&
            clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            now_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
            if (fe->heartbeat_last_ns == 0)
                fe->heartbeat_last_ns = now_ns;
            while (now_ns - fe->heartbeat_last_ns >= UINT64_C(500000000)) {
                fe->heartbeat++;
                fe->heartbeat_last_ns += UINT64_C(500000000);
            }
        } else if (fe->state != PCI_FE_STARTED) {
            fe->heartbeat_last_ns = 0;
        }
    }
    if (fe != NULL && fe->pe != NULL)
        while (doca_pe_progress(fe->pe) != 0) {
        }
}

int pci_fe_plug(struct pci_fe *fe)
{
    struct srdma_gemini_srdma_plug_msg plug;
    uint32_t remote_error = 0;
    doca_error_t result;
    int rc;

    if (fe == NULL || fe->state != PCI_FE_ABSENT)
        return -EBUSY;
    if (!gemini_server_ready(fe->gemini))
        return -ENOTCONN;
    fe->state = PCI_FE_PLUGGING;
    fe->generation++;
    result = create_endpoint(fe);
    if (result != DOCA_SUCCESS) {
        rc = -EIO;
        goto fail;
    }

    memset(&plug, 0, sizeof(plug));
    plug.rvf_id = 0;
    plug.doorbell_pages = VFIO_ADMINQ_UAR_MAX_ID + 1U;
    plug.max_qp_num = 128;
    memcpy(plug.netdev_mac, fe->mac, sizeof(fe->mac));
    plug.rsvd0[0] = (uint8_t)fe->vhca_id;
    plug.rsvd0[1] = (uint8_t)(fe->vhca_id >> 8);
    plug.rsvd1[0] = fe->generation;
    rc = gemini_server_send_config(fe->gemini, GEMINI_MSG_CFG_VDEV_PLUG,
                                   &plug, sizeof(plug), 10000,
                                   progress_cb, fe, &remote_error);
    if (rc != 0) {
        fprintf(stderr, "PLUG rejected: rc=%d remote_error=%u\n", rc,
                remote_error);
        goto fail;
    }

    fe->ready = true;
    fe->state = PCI_FE_PRESENT_STOPPED;
    printf("vfio-adminq device plugged: vhca_id=%u\n", fe->vhca_id);
    return 0;

fail:
    destroy_endpoint(fe);
    fe->state = PCI_FE_ABSENT;
    return rc;
}

static int send_stop(struct pci_fe *fe)
{
    uint16_t vdev_id = 0;
    uint32_t remote_error = 0;
    int rc;

    fe->state = PCI_FE_STOPPING;
    rc = gemini_server_send_config(fe->gemini, GEMINI_MSG_SRDMA_STOP,
                                   &vdev_id, sizeof(vdev_id), 10000,
                                   progress_cb, fe, &remote_error);
    fe->init_done = false;
    fe->pending_action = PCI_FE_ACTION_NONE;
    fe->state = PCI_FE_PRESENT_STOPPED;
    if (rc != 0)
        fprintf(stderr, "STOP failed: rc=%d remote_error=%u\n", rc,
                remote_error);
    return rc;
}

int pci_fe_unplug(struct pci_fe *fe, bool force)
{
    uint16_t vdev_id = 0;
    uint32_t remote_error = 0;
    int first_error = 0;
    int rc;

    if (fe == NULL)
        return -EINVAL;
    if (fe->state == PCI_FE_ABSENT)
        return 0;
    if (fe->state == PCI_FE_STARTED || fe->state == PCI_FE_STARTING) {
        rc = send_stop(fe);
        if (rc != 0)
            first_error = rc;
    }
    fe->state = PCI_FE_UNPLUGGING;
    if (gemini_server_ready(fe->gemini)) {
        rc = gemini_server_send_config(fe->gemini,
                                       GEMINI_MSG_CFG_VDEV_UNPLUG,
                                       &vdev_id, sizeof(vdev_id), 10000,
                                       progress_cb, fe, &remote_error);
        if (rc != 0 && first_error == 0)
            first_error = rc;
    } else if (!force) {
        fe->state = PCI_FE_PRESENT_STOPPED;
        return -ENOTCONN;
    }
    destroy_endpoint(fe);
    fe->state = PCI_FE_ABSENT;
    printf("vfio-adminq device unplugged%s\n",
           first_error == 0 ? "" : " after forced cleanup");
    return first_error;
}

void pci_fe_process_pending(struct pci_fe *fe)
{
    struct srdma_gemini_srdma_start_msg start;
    uint32_t remote_error = 0;
    int rc;

    if (fe == NULL || fe->pending_action == PCI_FE_ACTION_NONE)
        return;
    if (fe->pending_action == PCI_FE_ACTION_STOP) {
        if (fe->state == PCI_FE_STARTED || fe->state == PCI_FE_STARTING)
            (void)send_stop(fe);
        else
            fe->pending_action = PCI_FE_ACTION_NONE;
        return;
    }
    fe->pending_action = PCI_FE_ACTION_NONE;
    if (fe->state != PCI_FE_PRESENT_STOPPED)
        return;
    if (!fe->ready || !fe->bus_master_enable || fe->adminq_tx_addr == 0 ||
        fe->adminq_rx_addr == 0 || fe->asyncq_addr == 0 ||
        fe->adminq_depth != SRDMA_ADMINQ_DEPTH ||
        fe->asyncq_depth != SRDMA_AEQ_DEPTH ||
        (fe->adminq_tx_addr & 0xfffU) != 0 ||
        (fe->adminq_rx_addr & 0xfffU) != 0 ||
        (fe->asyncq_addr & 0xfffU) != 0) {
        fprintf(stderr, "START ignored: invalid BAR configuration or bus master disabled\n");
        return;
    }

    memset(&start, 0, sizeof(start));
    start.rvf_id = 0;
    start.pcie_port = 0;
    start.bdf = fe->bdf;
    start.txq_addr = fe->adminq_tx_addr;
    start.rxq_addr = fe->adminq_rx_addr;
    start.aeq_addr = fe->asyncq_addr;
    start.txq_depth = fe->adminq_depth;
    start.rxq_depth = fe->adminq_depth;
    start.aeq_depth = fe->asyncq_depth;
    fe->state = PCI_FE_STARTING;
    rc = gemini_server_send_config(fe->gemini, GEMINI_MSG_SRDMA_START,
                                   &start, sizeof(start), 10000,
                                   progress_cb, fe, &remote_error);
    if (rc == 0) {
        fe->init_done = true;
        fe->state = PCI_FE_STARTED;
        printf("vfio-adminq START succeeded: bdf=0x%04x txq=0x%" PRIx64 "\n",
               fe->bdf, fe->adminq_tx_addr);
    } else {
        fe->init_done = false;
        fe->state = PCI_FE_PRESENT_STOPPED;
        fprintf(stderr, "START failed: rc=%d remote_error=%u\n", rc,
                remote_error);
    }
}

const char *pci_fe_state_name(enum pci_fe_state state)
{
    static const char *const names[] = {
        [PCI_FE_ABSENT] = "ABSENT",
        [PCI_FE_PLUGGING] = "PLUGGING",
        [PCI_FE_PRESENT_STOPPED] = "PRESENT_STOPPED",
        [PCI_FE_STARTING] = "STARTING",
        [PCI_FE_STARTED] = "STARTED",
        [PCI_FE_STOPPING] = "STOPPING",
        [PCI_FE_UNPLUGGING] = "UNPLUGGING",
    };

    if ((unsigned int)state >= sizeof(names) / sizeof(names[0]) ||
        names[state] == NULL)
        return "UNKNOWN";
    return names[state];
}
