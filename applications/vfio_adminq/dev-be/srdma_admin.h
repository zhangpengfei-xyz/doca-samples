#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../common/srdma_admin_abi.h"

#define SRDMA_ADMIN_MAX_EQ 128U
#define SRDMA_ADMIN_MAX_PD 256U
#define SRDMA_ADMIN_MAX_MR 1024U
#define SRDMA_ADMIN_MAX_CQ 1024U
#define SRDMA_ADMIN_MAX_QP 1024U
#define SRDMA_ADMIN_MAX_UCTX 48U
#define SRDMA_ADMIN_MAX_GID 128U

struct srdma_admin_resource {
    bool allocated;
    uint32_t generation;
    uint32_t refs;
};

struct srdma_admin_eq {
    struct srdma_admin_resource resource;
    uint64_t addr;
    uint16_t vector;
    uint8_t log_depth;
};

struct srdma_admin_pd {
    struct srdma_admin_resource resource;
    uint16_t uctx;
};

struct srdma_admin_mr {
    struct srdma_admin_resource resource;
    uint32_t pd;
    uint64_t va;
    uint64_t length;
    uint32_t mkey;
};

struct srdma_admin_cq {
    struct srdma_admin_resource resource;
    uint16_t uctx;
    uint16_t eq;
    uint8_t log_depth;
};

struct srdma_admin_qp {
    struct srdma_admin_resource resource;
    uint32_t pd;
    uint32_t scq;
    uint32_t rcq;
    uint8_t type;
    uint8_t state;
    uint8_t log_mtu;
    uint8_t port;
    uint32_t qkey;
    uint32_t dest_qpn;
    uint32_t sq_psn;
    uint32_t rq_psn;
    uint32_t flow_label;
    uint16_t udp_sport;
    uint8_t access;
    uint8_t min_rnr;
    uint8_t rnr_retry;
    uint8_t timeout;
    uint8_t retry_cnt;
    uint8_t ra_res;
    uint8_t ra_req;
    uint8_t sgid_idx;
    uint8_t hop_limit;
    uint8_t traffic_class;
    uint8_t static_rate;
    uint8_t dscp;
    uint8_t eth_prio;
    uint8_t dgid[16];
    uint8_t dmac[6];
};

struct srdma_admin {
    struct srdma_admin_resource uctx[SRDMA_ADMIN_MAX_UCTX];
    struct srdma_admin_eq eq[SRDMA_ADMIN_MAX_EQ + 1];
    struct srdma_admin_pd pd[SRDMA_ADMIN_MAX_PD + 1];
    struct srdma_admin_mr mr[SRDMA_ADMIN_MAX_MR + 1];
    struct srdma_admin_cq cq[SRDMA_ADMIN_MAX_CQ + 1];
    struct srdma_admin_qp qp[SRDMA_ADMIN_MAX_QP + 1];
    bool gid[SRDMA_ADMIN_MAX_GID];
    bool netdev_up;
    bool monitor_enabled;
    uint32_t generation;
    uint64_t commands;
    uint64_t failures;
};

void srdma_admin_init(struct srdma_admin *admin, uint32_t generation);
void srdma_admin_reset(struct srdma_admin *admin, uint32_t generation);
enum srdma_admin_rc srdma_admin_execute(struct srdma_admin *admin,
                                        uint8_t opcode,
                                        const struct srdma_admin_entry *request,
                                        struct srdma_admin_entry *response);
