#pragma once

#include <stdint.h>

#define SRDMA_DB_RPC_SUCCESS 0
#define SRDMA_DB_RPC_ERROR 1
#define SRDMA_DB_MAX_MSGS 64

enum srdma_db_msg_type {
    SRDMA_DB_MSG_HOST_DB = 1,
};

struct srdma_db_msg {
    uint32_t type;
    uint32_t db_value;
    uint64_t user_data;
} __attribute__((__packed__, aligned(8)));

struct srdma_dpa_thread_arg {
    uint64_t dpa_db_comp;
    uint64_t dpa_producer;
} __attribute__((__packed__, aligned(8)));
