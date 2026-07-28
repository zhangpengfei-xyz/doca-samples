#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define SRDMA_UAR_IPC_DEFAULT_PATH "/run/vfio-adminq/srdma-uar"
#define SRDMA_UAR_IPC_DEPTH 1024U
#define SRDMA_UAR_IPC_MAGIC 0x52415553U
#define SRDMA_UAR_IPC_VERSION 1U

struct srdma_uar_event {
    uint16_t endpoint_id;
    uint16_t uctx_id;
    uint16_t offset;
    uint16_t width;
    uint64_t value;
    uint64_t generation;
};

struct srdma_uar_ring {
    uint32_t magic;
    uint32_t version;
    uint32_t depth;
    uint32_t reserved;
    _Atomic uint32_t producer;
    _Atomic uint32_t consumer;
    _Atomic uint32_t fatal;
    uint32_t pad;
    struct srdma_uar_event events[SRDMA_UAR_IPC_DEPTH];
};

struct srdma_uar_ipc {
    struct srdma_uar_ring *ring;
    int shm_fd;
    int socket_fd;
    int peer_fd;
    int event_fd;
    bool producer;
    char shm_path[108];
    char socket_path[108];
};

int srdma_uar_ipc_producer_init(struct srdma_uar_ipc *ipc,
                                const char *path);
int srdma_uar_ipc_producer_progress(struct srdma_uar_ipc *ipc);
int srdma_uar_ipc_consumer_init(struct srdma_uar_ipc *ipc,
                                const char *path);
int srdma_uar_ipc_push(struct srdma_uar_ipc *ipc,
                       const struct srdma_uar_event *event);
int srdma_uar_ipc_pop(struct srdma_uar_ipc *ipc,
                      struct srdma_uar_event *event);
int srdma_uar_ipc_event_fd(const struct srdma_uar_ipc *ipc);
void srdma_uar_ipc_mark_fatal(struct srdma_uar_ipc *ipc);
bool srdma_uar_ipc_is_fatal(const struct srdma_uar_ipc *ipc);
void srdma_uar_ipc_cleanup(struct srdma_uar_ipc *ipc);
