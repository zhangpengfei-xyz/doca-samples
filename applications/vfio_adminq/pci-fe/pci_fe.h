#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <doca_error.h>

#include "../common/srdma_uar_ipc.h"
#include "gemini_server.h"

struct doca_ctx;
struct doca_dev;
struct doca_dev_rep;
struct doca_devemu_pci_tlp_channel;
struct doca_devemu_pci_tlp_dev;
struct doca_devemu_pci_type;
struct doca_pe;

enum pci_fe_state {
    PCI_FE_ABSENT = 0,
    PCI_FE_PLUGGING,
    PCI_FE_PRESENT_STOPPED,
    PCI_FE_STARTING,
    PCI_FE_STARTED,
    PCI_FE_STOPPING,
    PCI_FE_UNPLUGGING,
};

enum pci_fe_pending_action {
    PCI_FE_ACTION_NONE = 0,
    PCI_FE_ACTION_START,
    PCI_FE_ACTION_STOP,
};

struct pci_fe {
    struct doca_dev *dev;
    struct doca_devemu_pci_type *pci_type;
    struct doca_dev_rep *rep;
    struct doca_devemu_pci_tlp_dev *tlp_dev;
    struct doca_pe *pe;
    struct doca_devemu_pci_tlp_channel *tlp_channel;
    struct doca_ctx *channel_ctx;
    struct gemini_server *gemini;

    enum pci_fe_state state;
    enum pci_fe_pending_action pending_action;
    bool pci_type_started;
    bool tlp_dev_started;
    bool channel_started;
    bool ready;
    bool init_done;
    bool bar_probe_low;
    bool bar_probe_high;
    bool memory_enable;
    bool bus_master_enable;

    uint8_t config_space[4096];
    uint64_t bar0_base;
    uint64_t adminq_tx_addr;
    uint64_t adminq_rx_addr;
    uint64_t asyncq_addr;
    uint32_t adminq_depth;
    uint32_t asyncq_depth;
    uint16_t vhca_id;
    uint16_t bdf;
    uint8_t mac[6];
    uint64_t generation;
    uint32_t heartbeat;
    uint64_t heartbeat_last_ns;
    struct srdma_uar_ipc uar_ipc;
};

doca_error_t pci_fe_init(struct pci_fe *fe, const char *pci_addr,
                         struct gemini_server *gemini,
                         const uint8_t mac[6], const char *uar_ipc_path);
void pci_fe_cleanup(struct pci_fe *fe);
void pci_fe_progress(struct pci_fe *fe);
int pci_fe_plug(struct pci_fe *fe);
int pci_fe_unplug(struct pci_fe *fe, bool force);
void pci_fe_process_pending(struct pci_fe *fe);
const char *pci_fe_state_name(enum pci_fe_state state);
