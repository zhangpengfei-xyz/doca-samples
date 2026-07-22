#pragma once

#include <stddef.h>
#include <stdint.h>

/* Host-visible PCI identity, kept compatible with io-engine SRDMA. */
#define VFIO_ADMINQ_PCI_VENDOR_ID 0x1e93U
#define VFIO_ADMINQ_PCI_DEVICE_ID 0x006aU
#define VFIO_ADMINQ_PCI_REVISION 0U
#define VFIO_ADMINQ_PCI_CLASS_CODE 0x020000U

#define VFIO_ADMINQ_PCI_TYPE_NAME "custom_pci_dev"
#define VFIO_ADMINQ_DEFAULT_DOCA_PCI_ADDR "0000:03:00.0"

/* DOCA type layout. The Host-visible BAR size remains 64 KiB. */
#define VFIO_ADMINQ_BAR_ID 0U
#define VFIO_ADMINQ_DOCA_BAR0_LOG_SIZE 20U
#define VFIO_ADMINQ_HOST_BAR0_SIZE 0x10000U
#define VFIO_ADMINQ_TLP_REGION_OFFSET 0x0000U
#define VFIO_ADMINQ_TLP_REGION_SIZE 0x8000U
#define VFIO_ADMINQ_DB_REGION_OFFSET 0x8000U
#define VFIO_ADMINQ_DB_REGION_SIZE 0x1000U
#define VFIO_ADMINQ_DB_LOG_SIZE 1U
#define VFIO_ADMINQ_DB_STRIDE_LOG_SIZE 3U
#define VFIO_ADMINQ_DB_COUNT 256U
#define VFIO_ADMINQ_DB_ID 0U

/* BAR0 register ABI from io-engine srdma_bfa.h. */
#define SRDMA_BFA_PCI_VERSION 0x00U
#define SRDMA_BFA_PCI_DEV_READY 0x04U
#define SRDMA_BFA_MAX_QP_NUM 0x08U
#define SRDMA_BFA_PCI_MAX_VECTORS 0x0cU
#define SRDMA_BFA_PCI_DEV_MACADDR 0x10U
#define SRDMA_BFA_PCI_DEV_CTRL 0x20U
#define SRDMA_BFA_PCI_DEV_CTRL_CMD_RESET (1U << 0)
#define SRDMA_BFA_PCI_DEV_CTRL_CMD_INIT_DONE (1U << 1)
#define SRDMA_BFA_PCI_DEV_STATUS 0x24U
#define SRDMA_BFA_PCI_DEV_STATUS_INIT_DONE (1U << 1)
#define SRDMA_BFA_PCI_DEV_ADMIN_TXQ_BASEADDR_LO 0x40U
#define SRDMA_BFA_PCI_DEV_ADMIN_TXQ_BASEADDR_HI 0x44U
#define SRDMA_BFA_PCI_DEV_ADMIN_RXQ_BASEADDR_LO 0x48U
#define SRDMA_BFA_PCI_DEV_ADMIN_RXQ_BASEADDR_HI 0x4cU
#define SRDMA_BFA_PCI_DEV_ASYNCQ_BASEADDR_LO 0x50U
#define SRDMA_BFA_PCI_DEV_ASYNCQ_BASEADDR_HI 0x54U
#define SRDMA_BFA_PCI_DEV_ADMINQ_DEPTH 0x58U
#define SRDMA_BFA_PCI_DEV_ASYNCQ_DEPTH 0x5cU
#define SRDMA_BFA_PCI_DEV_DIAG 0x80U
#define SRDMA_BFA_PCI_DEV_DIAG_SIZE 0x40U
#define SRDMA_BAR0_DOORBELL_OFFSET VFIO_ADMINQ_DB_REGION_OFFSET

/* Gemini v5 wire ABI. */
#define SRDMA_GEMINI_PAYLOAD_SIZE 4096U
#define SRDMA_GEMINI_MSG_HDR_SIZE 16U
#define SRDMA_GEMINI_DEFAULT_SOCKET "/var/tmp/bes2/bes2-server.sock"

enum vfio_adminq_gemini_request {
    GEMINI_HELLO = 0,
    GEMINI_SCAN = 1,
    GEMINI_CONFIG_UPDATE = 3,
    GEMINI_MIGRATION = 6,
    GEMINI_READY = 7,
};

enum vfio_adminq_gemini_client_type {
    GEMINI_MSG_TYPE_VIRTIO_NET = 0,
    GEMINI_MSG_TYPE_VIRTIO_BLK = 1,
    GEMINI_MSG_TYPE_VIRTIO_MSG = 2,
    GEMINI_MSG_TYPE_BYTEVISOR = 3,
    GEMINI_MSG_TYPE_SRDMA = 4,
};

#define GEMINI_MSG_VERSION_CUR 5U
#define GEMINI_MSG_F_VERSION_MASK 0xffU
#define GEMINI_MSG_F_NEEDS_REPLY (1U << 30)
#define GEMINI_MSG_F_HAS_ERROR (1U << 30)
#define GEMINI_MSG_F_IS_REPLY (1U << 31)
#define GEMINI_MSG_F_CFG_TYPE_MASK 0xff00U
#define GEMINI_MSG_F_CFG_TYPE_SHIFT 8U
#define GEMINI_MSG_F_CFG_TYPE(flags) \
    (((flags) & GEMINI_MSG_F_CFG_TYPE_MASK) >> GEMINI_MSG_F_CFG_TYPE_SHIFT)

#define GEMINI_MSG_CFG_VDEV_PLUG 0x00U
#define GEMINI_MSG_CFG_VDEV_UNPLUG 0x01U
#define GEMINI_MSG_SRDMA_START 0x06U
#define GEMINI_MSG_SRDMA_STOP 0x07U

#define GEMINI_MSG_ERR_INVALID_DATA_LEN 1U
#define GEMINI_MSG_ERR_INVALID_PAYLOAD 2U
#define GEMINI_MSG_ERR_REQUEST_FAIL 5U

#define DPU_FPGA_SRDMA_MSIX_ARRAY_NR 8U

struct srdma_gemini_hello_msg {
    uint8_t type;
} __attribute__((packed));

struct srdma_gemini_srdma_plug_msg {
    uint16_t rvf_id;
    uint16_t doorbell_pages;
    uint32_t max_qp_num;
    uint8_t netdev_mac[6];
    uint8_t rsvd0[2]; /* Existing ABI: little-endian endpoint VHCA ID. */
    uint16_t netdev_pvf_id;
    uint16_t netdev_feq0_id;
    uint16_t netdev_vm_id;
    uint16_t netdev_vp_id;
    uint64_t rsvd1[5];
    uint64_t msix_bitmap[DPU_FPGA_SRDMA_MSIX_ARRAY_NR];
} __attribute__((packed));

struct srdma_gemini_srdma_start_msg {
    uint16_t rvf_id;
    uint8_t rsvd0[3];
    uint8_t pcie_port;
    uint16_t bdf;
    uint64_t txq_addr;
    uint64_t rxq_addr;
    uint64_t aeq_addr;
    uint32_t txq_depth;
    uint32_t rxq_depth;
    uint32_t aeq_depth;
    uint32_t rsvd1[18];
} __attribute__((packed));

struct srdma_gemini_msg {
    uint32_t request;
    uint32_t request_id;
    uint32_t flags;
    union {
        uint32_t data_len;
        uint32_t errcode;
    };
    union {
        uint8_t u8;
        uint16_t u16;
        struct srdma_gemini_hello_msg hello;
        struct srdma_gemini_srdma_plug_msg srdma_plug;
        struct srdma_gemini_srdma_start_msg srdma_start;
        uint8_t raw[SRDMA_GEMINI_PAYLOAD_SIZE];
    } payload;
};

/* Verified single-command AdminQ test ABI. */
#define SRDMA_ADMINQ_TEST_MAGIC 0x51445253U
#define SRDMA_ADMINQ_TEST_VERSION 1U
#define SRDMA_ADMINQ_TEST_LEN 256U
#define SRDMA_ADMINQ_TEST_TEXT_LEN 64U

enum srdma_adminq_test_status {
    SRDMA_ADMINQ_TEST_EMPTY = 0,
    SRDMA_ADMINQ_TEST_GUEST_READY = 1,
    SRDMA_ADMINQ_TEST_HOST_DONE = 2,
};

struct srdma_adminq_test_msg {
    uint32_t magic;
    uint16_t version;
    uint16_t status;
    uint32_t guest_seq;
    uint32_t host_seq;
    uint64_t guest_iova;
    uint64_t host_seen_doorbells;
    char guest_text[SRDMA_ADMINQ_TEST_TEXT_LEN];
    char host_text[SRDMA_ADMINQ_TEST_TEXT_LEN];
    uint8_t reserved[96];
} __attribute__((packed));

_Static_assert(offsetof(struct srdma_gemini_msg, payload) ==
                   SRDMA_GEMINI_MSG_HDR_SIZE,
               "unexpected Gemini header size");
_Static_assert(sizeof(struct srdma_gemini_srdma_plug_msg) == 128,
               "unexpected Gemini SRDMA PLUG size");
_Static_assert(sizeof(struct srdma_gemini_srdma_start_msg) == 116,
               "unexpected Gemini SRDMA START size");
_Static_assert(sizeof(struct srdma_adminq_test_msg) == SRDMA_ADMINQ_TEST_LEN,
               "unexpected AdminQ test message size");
