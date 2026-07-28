#pragma once

#include <stddef.h>
#include <stdint.h>

#define SRDMA_ADMIN_ENTRY_SIZE 128U
#define SRDMA_ADMIN_PAYLOAD_DWORDS 30U

struct srdma_admin_entry {
    uint64_t hdr;
    uint32_t dw[SRDMA_ADMIN_PAYLOAD_DWORDS];
} __attribute__((packed));

enum srdma_admin_opcode {
    SRDMA_ADMIN_OP_GET_CAP = 0x01,
    SRDMA_ADMIN_OP_GET_CAP_EXT = 0x02,
    SRDMA_ADMIN_OP_HEALTH_CHECK = 0x03,
    SRDMA_ADMIN_OP_NETDEV_UP = 0x08,
    SRDMA_ADMIN_OP_NETDEV_DOWN = 0x09,
    SRDMA_ADMIN_OP_ADD_GID = 0x0e,
    SRDMA_ADMIN_OP_DEL_GID = 0x0f,
    SRDMA_ADMIN_OP_CREATE_EQ = 0x10,
    SRDMA_ADMIN_OP_DESTROY_EQ = 0x11,
    SRDMA_ADMIN_OP_ALLOC_PD = 0x14,
    SRDMA_ADMIN_OP_DEALLOC_PD = 0x15,
    SRDMA_ADMIN_OP_REG_MR = 0x18,
    SRDMA_ADMIN_OP_DEREG_MR = 0x19,
    SRDMA_ADMIN_OP_CREATE_CQ = 0x30,
    SRDMA_ADMIN_OP_DESTROY_CQ = 0x31,
    SRDMA_ADMIN_OP_CREATE_QP = 0x40,
    SRDMA_ADMIN_OP_DESTROY_QP = 0x41,
    SRDMA_ADMIN_OP_QUERY_QP = 0x42,
    SRDMA_ADMIN_OP_ALLOC_UCTX = 0x50,
    SRDMA_ADMIN_OP_DEALLOC_UCTX = 0x51,
    SRDMA_ADMIN_OP_GET_STATS = 0x60,
    SRDMA_ADMIN_OP_GET_STATS_EXT = 0x61,
    SRDMA_ADMIN_OP_GET_STATS_EXT2 = 0x64,
    SRDMA_ADMIN_OP_GET_STATS_EXT3 = 0x65,
    SRDMA_ADMIN_OP_ENABLE_MS_MONITOR = 0x70,
    SRDMA_ADMIN_OP_DISABLE_MS_MONITOR = 0x71,
    SRDMA_ADMIN_OP_RST2INIT_QP = 0x80,
    SRDMA_ADMIN_OP_INIT2INIT_QP = 0x81,
    SRDMA_ADMIN_OP_INIT2RTR_QP = 0x82,
    SRDMA_ADMIN_OP_RTR2RTS_QP = 0x83,
    SRDMA_ADMIN_OP_RTS2RTS_QP = 0x84,
    SRDMA_ADMIN_OP_2ERR_QP = 0x89,
    SRDMA_ADMIN_OP_2RST_QP = 0x8a,
};

enum srdma_admin_rc {
    SRDMA_ADMIN_RC_SUCC = 0,
    SRDMA_ADMIN_RC_HW_ERR = 1,
    SRDMA_ADMIN_RC_FW_ERR = 2,
    SRDMA_ADMIN_RC_OP_NOT_SUPP = 3,
    SRDMA_ADMIN_RC_INVALID_ARG = 4,
    SRDMA_ADMIN_RC_INVALID_IDX = 5,
    SRDMA_ADMIN_RC_NO_RESOURCE = 6,
    SRDMA_ADMIN_RC_RESOURCE_BUSY = 7,
    SRDMA_ADMIN_RC_FATAL_ERR = 8,
};

#define SRDMA_ADMIN_HDR_ID(h) ((uint8_t)((h) & 0xffU))
#define SRDMA_ADMIN_HDR_SIZE(h) ((uint8_t)(((h) >> 8) & 0xffU))
#define SRDMA_ADMIN_HDR_OPCODE(h) ((uint8_t)(((h) >> 16) & 0xffU))
#define SRDMA_ADMIN_RSP_HEADER(id, ci, rc, owner) \
    ((uint64_t)(id) | ((uint64_t)SRDMA_ADMIN_ENTRY_SIZE << 8) | \
     ((uint64_t)(ci) << 16) | ((uint64_t)(rc) << 56) | \
     ((uint64_t)(!!(owner)) << 63))

static inline uint32_t srdma_admin_get(const struct srdma_admin_entry *entry,
                                       unsigned int dw, unsigned int bit,
                                       unsigned int width)
{
    uint32_t mask = width == 32 ? UINT32_MAX : ((1U << width) - 1U);
    return (entry->dw[dw] >> bit) & mask;
}

static inline void srdma_admin_set(struct srdma_admin_entry *entry,
                                   unsigned int dw, unsigned int bit,
                                   unsigned int width, uint32_t value)
{
    uint32_t mask = width == 32 ? UINT32_MAX : ((1U << width) - 1U);
    entry->dw[dw] = (entry->dw[dw] & ~(mask << bit)) |
                    ((value & mask) << bit);
}

static inline uint64_t srdma_admin_get64(const struct srdma_admin_entry *entry,
                                         unsigned int dw)
{
    return (uint64_t)entry->dw[dw] | ((uint64_t)entry->dw[dw + 1] << 32);
}

static inline void srdma_admin_set64(struct srdma_admin_entry *entry,
                                     unsigned int dw, uint64_t value)
{
    entry->dw[dw] = (uint32_t)value;
    entry->dw[dw + 1] = (uint32_t)(value >> 32);
}

_Static_assert(sizeof(struct srdma_admin_entry) == SRDMA_ADMIN_ENTRY_SIZE,
               "sRDMA AdminQ entry ABI mismatch");
