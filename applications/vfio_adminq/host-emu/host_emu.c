#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/pci_regs.h>
#include <linux/vfio.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../common/vfio_adminq_abi.h"

#define SRDMA_DEFAULT_BDF "0000:78:00.0"
#define SRDMA_DEFAULT_DMA_SIZE 4096
#define SRDMA_DEFAULT_IOVA 0x100000000ULL
#define SRDMA_DEFAULT_DUMP_LEN 64
#define SRDMA_ADMIN_QUEUE_STRIDE 0x100
#define SRDMA_ADMIN_TXQ_IOVA_OFFSET 0
#define SRDMA_ADMIN_RXQ_IOVA_OFFSET SRDMA_ADMIN_QUEUE_STRIDE
#define SRDMA_ASYNCQ_IOVA_OFFSET (SRDMA_ADMIN_QUEUE_STRIDE * 2)
#define SRDMA_DEFAULT_ADMINQ_DEPTH 16
#define SRDMA_DEFAULT_ASYNCQ_DEPTH 16
#define SRDMA_DEVICE_START_TIMEOUT_SEC 30
#define SRDMA_ADMINQ_ACK_TIMEOUT_SEC 10

struct srdma_driver_opts {
    char bdf[32];
    size_t size;
    uint64_t iova;
    size_t dump_len;
    uint32_t doorbell_value;
    uint32_t adminq_depth;
    uint32_t asyncq_depth;
};

struct srdma_vfio {
    int container_fd;
    int group_fd;
    int device_fd;
    char group_name[32];
};

static volatile sig_atomic_t g_stop;

static uint64_t monotonic_seconds(void);

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "\n"
           "Options:\n"
           "  --bdf <bdf>             PCI BDF (default: %s)\n"
           "  --size <n>              DMA buffer size (default: %u)\n"
           "  --iova <addr>           VFIO DMA IOVA (default: 0x%" PRIx64 ")\n"
           "  --dump-len <n>          bytes to dump (default: %u)\n"
           "  -h, --help              show this help\n",
           prog, SRDMA_DEFAULT_BDF, SRDMA_DEFAULT_DMA_SIZE,
           (uint64_t)SRDMA_DEFAULT_IOVA, SRDMA_DEFAULT_DUMP_LEN);
}

static void stop_signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        g_stop = 1;
    }
}

static int normalize_bdf(const char *input, char *out, size_t out_len)
{
    unsigned int domain;
    unsigned int bus;
    unsigned int dev;
    unsigned int func;

    if (sscanf(input, "%x:%x:%x.%x", &domain, &bus, &dev, &func) == 4) {
        if (snprintf(out, out_len, "%04x:%02x:%02x.%u", domain, bus, dev,
                     func) >= (int)out_len) {
            return -1;
        }
        return 0;
    }

    if (sscanf(input, "%x:%x.%x", &bus, &dev, &func) == 3) {
        if (snprintf(out, out_len, "0000:%02x:%02x.%u", bus, dev,
                     func) >= (int)out_len) {
            return -1;
        }
        return 0;
    }

    fprintf(stderr, "invalid BDF: %s\n", input);
    return -1;
}

static int parse_u64(const char *name, const char *value, uint64_t *out)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        return -1;
    }

    *out = (uint64_t)parsed;
    return 0;
}

static int parse_size(const char *name, const char *value, size_t *out)
{
    uint64_t parsed;

    if (parse_u64(name, value, &parsed) != 0 || parsed == 0 ||
        parsed > SIZE_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        return -1;
    }

    *out = (size_t)parsed;
    return 0;
}

static int parse_args(int argc, char **argv, struct srdma_driver_opts *opts)
{
    static const struct option long_options[] = {
        {"bdf", required_argument, NULL, 'b'},
        {"size", required_argument, NULL, 's'},
        {"iova", required_argument, NULL, 'i'},
        {"dump-len", required_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int opt;

    while ((opt = getopt_long(argc, argv, "h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'b':
            if (normalize_bdf(optarg, opts->bdf, sizeof(opts->bdf)) != 0) {
                return -1;
            }
            break;
        case 's':
            if (parse_size("size", optarg, &opts->size) != 0) {
                return -1;
            }
            break;
        case 'i':
            if (parse_u64("iova", optarg, &opts->iova) != 0) {
                return -1;
            }
            break;
        case 'd':
            if (parse_size("dump-len", optarg, &opts->dump_len) != 0) {
                return -1;
            }
            break;
        case 'h':
            return 1;
        default:
            return -1;
        }
    }

    if (optind != argc) {
        fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
        return -1;
    }

    return 0;
}

static void default_opts(struct srdma_driver_opts *opts)
{
    memset(opts, 0, sizeof(*opts));
    (void)snprintf(opts->bdf, sizeof(opts->bdf), "%s", SRDMA_DEFAULT_BDF);
    opts->size = SRDMA_DEFAULT_DMA_SIZE;
    opts->iova = SRDMA_DEFAULT_IOVA;
    opts->dump_len = SRDMA_DEFAULT_DUMP_LEN;
    opts->doorbell_value = 1;
    opts->adminq_depth = SRDMA_DEFAULT_ADMINQ_DEPTH;
    opts->asyncq_depth = SRDMA_DEFAULT_ASYNCQ_DEPTH;
}

static size_t round_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static int read_iommu_group(const char *bdf, char *group, size_t group_len)
{
    char path[PATH_MAX];
    char link_target[PATH_MAX];
    ssize_t len;
    const char *base;

    if (snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group",
                 bdf) >= (int)sizeof(path)) {
        return -1;
    }

    len = readlink(path, link_target, sizeof(link_target) - 1);
    if (len < 0) {
        perror("readlink iommu_group");
        return -1;
    }
    link_target[len] = '\0';

    base = strrchr(link_target, '/');
    base = base == NULL ? link_target : base + 1;
    if (snprintf(group, group_len, "%s", base) >= (int)group_len) {
        return -1;
    }

    return 0;
}

static int open_vfio(const struct srdma_driver_opts *opts,
                     struct srdma_vfio *vfio)
{
    struct vfio_group_status group_status = {
        .argsz = sizeof(group_status),
    };
    struct vfio_device_info device_info = {
        .argsz = sizeof(device_info),
    };
    char group_path[PATH_MAX];
    int api_version;

    vfio->container_fd = -1;
    vfio->group_fd = -1;
    vfio->device_fd = -1;

    if (read_iommu_group(opts->bdf, vfio->group_name,
                         sizeof(vfio->group_name)) != 0) {
        return -1;
    }

    vfio->container_fd = open("/dev/vfio/vfio", O_RDWR);
    if (vfio->container_fd < 0) {
        perror("open /dev/vfio/vfio");
        return -1;
    }

    api_version = ioctl(vfio->container_fd, VFIO_GET_API_VERSION);
    if (api_version != VFIO_API_VERSION) {
        fprintf(stderr, "unsupported VFIO API version: %d\n", api_version);
        return -1;
    }

    if (!ioctl(vfio->container_fd, VFIO_CHECK_EXTENSION,
               VFIO_TYPE1_IOMMU)) {
        fprintf(stderr, "VFIO_TYPE1_IOMMU is not supported\n");
        return -1;
    }

    if (snprintf(group_path, sizeof(group_path), "/dev/vfio/%s",
                 vfio->group_name) >= (int)sizeof(group_path)) {
        return -1;
    }

    vfio->group_fd = open(group_path, O_RDWR);
    if (vfio->group_fd < 0) {
        perror("open VFIO group");
        return -1;
    }

    if (ioctl(vfio->group_fd, VFIO_GROUP_GET_STATUS, &group_status) != 0) {
        perror("VFIO_GROUP_GET_STATUS");
        return -1;
    }
    if ((group_status.flags & VFIO_GROUP_FLAGS_VIABLE) == 0) {
        fprintf(stderr, "VFIO group %s is not viable\n", vfio->group_name);
        return -1;
    }

    if (ioctl(vfio->group_fd, VFIO_GROUP_SET_CONTAINER,
              &vfio->container_fd) != 0) {
        perror("VFIO_GROUP_SET_CONTAINER");
        return -1;
    }

    if (ioctl(vfio->container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) != 0) {
        perror("VFIO_SET_IOMMU");
        return -1;
    }

    vfio->device_fd = ioctl(vfio->group_fd, VFIO_GROUP_GET_DEVICE_FD,
                            opts->bdf);
    if (vfio->device_fd < 0) {
        perror("VFIO_GROUP_GET_DEVICE_FD");
        return -1;
    }

    if (ioctl(vfio->device_fd, VFIO_DEVICE_GET_INFO, &device_info) != 0) {
        perror("VFIO_DEVICE_GET_INFO");
        return -1;
    }
    if ((device_info.flags & VFIO_DEVICE_FLAGS_PCI) == 0) {
        fprintf(stderr, "%s is not a VFIO PCI device\n", opts->bdf);
        return -1;
    }

    printf("vfio opened: bdf=%s group=%s regions=%u irqs=%u\n", opts->bdf,
           vfio->group_name, device_info.num_regions, device_info.num_irqs);
    return 0;
}

static int set_pci_bus_master(int device_fd)
{
    struct vfio_region_info config = {
        .argsz = sizeof(config),
        .index = VFIO_PCI_CONFIG_REGION_INDEX,
    };
    uint16_t command;
    uint16_t new_command;
    off_t command_off;

    if (ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &config) != 0) {
        perror("VFIO_DEVICE_GET_REGION_INFO config");
        return -1;
    }

    command_off = (off_t)(config.offset + PCI_COMMAND);
    if (pread(device_fd, &command, sizeof(command), command_off) !=
        (ssize_t)sizeof(command)) {
        perror("pread PCI_COMMAND");
        return -1;
    }

    new_command = command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    if (pwrite(device_fd, &new_command, sizeof(new_command), command_off) !=
        (ssize_t)sizeof(new_command)) {
        perror("pwrite PCI_COMMAND");
        return -1;
    }

    printf("pci command: old=0x%04x new=0x%04x\n", command, new_command);
    return 0;
}

static void *alloc_dma_buffer(size_t size)
{
    void *buf;

    buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (buf == MAP_FAILED) {
        perror("mmap DMA buffer");
        return NULL;
    }

    memset(buf, 0x11, size);
    if (size >= 16) {
        memcpy(buf, "srdma-vfio-buf", 14);
    }
    return buf;
}

static int map_dma_buffer(int container_fd, void *buf, uint64_t iova,
                          size_t size)
{
    struct vfio_iommu_type1_dma_map map = {
        .argsz = sizeof(map),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr = (uintptr_t)buf,
        .iova = iova,
        .size = size,
    };

    if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &map) != 0) {
        perror("VFIO_IOMMU_MAP_DMA");
        return -1;
    }

    return 0;
}

static void unmap_dma_buffer(int container_fd, uint64_t iova, size_t size)
{
    struct vfio_iommu_type1_dma_unmap unmap = {
        .argsz = sizeof(unmap),
        .iova = iova,
        .size = size,
    };

    if (container_fd >= 0 &&
        ioctl(container_fd, VFIO_IOMMU_UNMAP_DMA, &unmap) != 0) {
        perror("VFIO_IOMMU_UNMAP_DMA");
    }
}

static void bar0_write32(void *bar, uint32_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)((uint8_t *)bar + offset);

    *reg = value;
}

static uint32_t bar0_read32(void *bar, uint32_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)((uint8_t *)bar + offset);

    return *reg;
}

static void bar0_write64_split(void *bar, uint32_t lo_offset,
                               uint32_t hi_offset, uint64_t value)
{
    bar0_write32(bar, lo_offset, (uint32_t)value);
    bar0_write32(bar, hi_offset, (uint32_t)(value >> 32));
}

static int wait_for_device_started(void *bar)
{
    uint64_t start = monotonic_seconds();
    uint32_t status;

    printf("waiting for SRDMA device status INIT_DONE bit\n");
    while (!g_stop) {
        status = bar0_read32(bar, SRDMA_BFA_PCI_DEV_STATUS);
        if ((status & SRDMA_BFA_PCI_DEV_STATUS_INIT_DONE) != 0) {
            printf("srdma device started: status=0x%08x\n", status);
            return 0;
        }
        if (monotonic_seconds() - start >= SRDMA_DEVICE_START_TIMEOUT_SEC) {
            fprintf(stderr, "timeout waiting for SRDMA device status: "
                    "last_status=0x%08x\n", status);
            return -1;
        }
        usleep(100 * 1000);
    }

    return -1;
}

static int stop_device(void *bar)
{
    uint64_t start = monotonic_seconds();
    uint32_t status;

    bar0_write32(bar, SRDMA_BFA_PCI_DEV_CTRL,
                 SRDMA_BFA_PCI_DEV_CTRL_CMD_RESET);
    __sync_synchronize();
    while (!g_stop) {
        status = bar0_read32(bar, SRDMA_BFA_PCI_DEV_STATUS);
        if ((status & SRDMA_BFA_PCI_DEV_STATUS_INIT_DONE) == 0) {
            printf("srdma device stopped: status=0x%08x\n", status);
            return 0;
        }
        if (monotonic_seconds() - start >= SRDMA_ADMINQ_ACK_TIMEOUT_SEC) {
            fprintf(stderr, "timeout waiting for SRDMA device stop: "
                    "last_status=0x%08x\n", status);
            return -1;
        }
        usleep(100 * 1000);
    }
    return -1;
}

static void write_adminq_test_msg(void *dma_buf, uint64_t txq_iova,
                                  uint32_t doorbell_value)
{
    struct srdma_adminq_test_msg *msg = dma_buf;

    memset(msg, 0, sizeof(*msg));
    msg->magic = SRDMA_ADMINQ_TEST_MAGIC;
    msg->version = SRDMA_ADMINQ_TEST_VERSION;
    msg->status = SRDMA_ADMINQ_TEST_GUEST_READY;
    msg->guest_seq = doorbell_value;
    msg->guest_iova = txq_iova;
    snprintf(msg->guest_text, sizeof(msg->guest_text),
             "guest adminq message before doorbell %u", doorbell_value);
    __sync_synchronize();

    printf("adminq test message written: iova=0x%" PRIx64
           " len=%zu guest_seq=%u\n",
           txq_iova, sizeof(*msg), msg->guest_seq);
}

static int wait_for_adminq_host_update(void *dma_buf)
{
    const struct srdma_adminq_test_msg *msg = dma_buf;
    uint64_t start = monotonic_seconds();

    printf("waiting for host adminq DMA update\n");
    while (!g_stop) {
        __sync_synchronize();
        if (msg->magic == SRDMA_ADMINQ_TEST_MAGIC &&
            msg->version == SRDMA_ADMINQ_TEST_VERSION &&
            msg->status == SRDMA_ADMINQ_TEST_HOST_DONE) {
            printf("adminq host update received: host_seq=%u "
                   "host_seen_doorbells=%" PRIu64 " host_text=\"%.*s\"\n",
                   msg->host_seq, msg->host_seen_doorbells,
                   (int)sizeof(msg->host_text), msg->host_text);
            return 0;
        }
        if (monotonic_seconds() - start >= SRDMA_ADMINQ_ACK_TIMEOUT_SEC) {
            fprintf(stderr, "timeout waiting for host adminq DMA update: "
                    "status=%u host_seq=%u\n",
                    msg->status, msg->host_seq);
            return -1;
        }
        usleep(100 * 1000);
    }

    return -1;
}

static int configure_adminq_and_start_device(
    const struct srdma_driver_opts *opts,
    int device_fd,
    void *dma_buf,
    size_t mapped_size)
{
    struct vfio_region_info bar0 = {
        .argsz = sizeof(bar0),
        .index = VFIO_PCI_BAR0_REGION_INDEX,
    };
    uint64_t txq_iova = opts->iova + SRDMA_ADMIN_TXQ_IOVA_OFFSET;
    uint64_t rxq_iova = opts->iova + SRDMA_ADMIN_RXQ_IOVA_OFFSET;
    uint64_t asyncq_iova = opts->iova + SRDMA_ASYNCQ_IOVA_OFFSET;
    size_t required_size = SRDMA_ASYNCQ_IOVA_OFFSET + SRDMA_ADMIN_QUEUE_STRIDE;
    void *bar;

    if (mapped_size < required_size) {
        fprintf(stderr, "mapped DMA buffer is too small for admin queues: "
                "%zu < %zu\n", mapped_size, required_size);
        return -1;
    }

    if (ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0) != 0) {
        perror("VFIO_DEVICE_GET_REGION_INFO BAR0");
        return -1;
    }
    if ((bar0.flags & VFIO_REGION_INFO_FLAG_MMAP) == 0 ||
        bar0.size < SRDMA_BAR0_DOORBELL_OFFSET + sizeof(uint32_t)) {
        fprintf(stderr, "BAR0 is not mmap-capable or too small\n");
        return -1;
    }

    bar = mmap(NULL, bar0.size, PROT_READ | PROT_WRITE, MAP_SHARED,
               device_fd, (off_t)bar0.offset);
    if (bar == MAP_FAILED) {
        perror("mmap BAR0");
        return -1;
    }

    bar0_write64_split(bar, SRDMA_BFA_PCI_DEV_ADMIN_TXQ_BASEADDR_LO,
                       SRDMA_BFA_PCI_DEV_ADMIN_TXQ_BASEADDR_HI, txq_iova);
    bar0_write64_split(bar, SRDMA_BFA_PCI_DEV_ADMIN_RXQ_BASEADDR_LO,
                       SRDMA_BFA_PCI_DEV_ADMIN_RXQ_BASEADDR_HI, rxq_iova);
    bar0_write64_split(bar, SRDMA_BFA_PCI_DEV_ASYNCQ_BASEADDR_LO,
                       SRDMA_BFA_PCI_DEV_ASYNCQ_BASEADDR_HI, asyncq_iova);
    bar0_write32(bar, SRDMA_BFA_PCI_DEV_ADMINQ_DEPTH, opts->adminq_depth);
    bar0_write32(bar, SRDMA_BFA_PCI_DEV_ASYNCQ_DEPTH, opts->asyncq_depth);
    __sync_synchronize();
    bar0_write32(bar, SRDMA_BFA_PCI_DEV_CTRL,
                 SRDMA_BFA_PCI_DEV_CTRL_CMD_INIT_DONE);
    __sync_synchronize();

    printf("srdma device start triggered: txq=0x%" PRIx64
           " rxq=0x%" PRIx64 " asyncq=0x%" PRIx64
           " adminq_depth=%u asyncq_depth=%u\n",
           txq_iova, rxq_iova, asyncq_iova,
           opts->adminq_depth, opts->asyncq_depth);

    if (wait_for_device_started(bar) != 0) {
        goto out_unmap;
    }

    write_adminq_test_msg(dma_buf, txq_iova, opts->doorbell_value);
    bar0_write32(bar, SRDMA_BAR0_DOORBELL_OFFSET, opts->doorbell_value);
    __sync_synchronize();
    printf("doorbell rung: bar0_offset=0x%x value=%u\n",
           SRDMA_BAR0_DOORBELL_OFFSET, opts->doorbell_value);

    if (wait_for_adminq_host_update(dma_buf) != 0) {
        goto out_unmap;
    }

    if (stop_device(bar) != 0) {
        goto out_unmap;
    }

    if (munmap(bar, bar0.size) != 0) {
        perror("munmap BAR0");
        return -1;
    }

    return 0;

out_unmap:
    (void)stop_device(bar);
    if (munmap(bar, bar0.size) != 0) {
        perror("munmap BAR0");
    }
    return -1;
}

static uint64_t monotonic_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec;
}

static void dump_prefix(const char *label, const uint8_t *buf, size_t len,
                        size_t dump_len)
{
    size_t n = len < dump_len ? len : dump_len;

    printf("%s", label);
    for (size_t i = 0; i < n; i++) {
        printf(" %02x", buf[i]);
    }
    if (n < len) {
        printf(" ...");
    }
    printf("\n");
}

static void close_vfio(struct srdma_vfio *vfio)
{
    if (vfio->device_fd >= 0) {
        close(vfio->device_fd);
        vfio->device_fd = -1;
    }
    if (vfio->group_fd >= 0) {
        close(vfio->group_fd);
        vfio->group_fd = -1;
    }
    if (vfio->container_fd >= 0) {
        close(vfio->container_fd);
        vfio->container_fd = -1;
    }
}

int main(int argc, char **argv)
{
    struct srdma_driver_opts opts;
    struct srdma_vfio vfio;
    long page_size;
    size_t map_size;
    void *buf = NULL;
    int parse_result;
    int ret = 1;

    default_opts(&opts);
    parse_result = parse_args(argc, argv, &opts);
    if (parse_result > 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (parse_result < 0) {
        print_usage(argv[0]);
        return 2;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        perror("sysconf page size");
        return 1;
    }
    map_size = round_up(opts.size, (size_t)page_size);
    if (opts.iova % (uint64_t)page_size != 0) {
        fprintf(stderr, "iova must be page aligned: 0x%" PRIx64 "\n",
                opts.iova);
        return 2;
    }

    signal(SIGINT, stop_signal_handler);
    signal(SIGTERM, stop_signal_handler);

    if (open_vfio(&opts, &vfio) != 0) {
        goto out;
    }
    if (set_pci_bus_master(vfio.device_fd) != 0) {
        goto out;
    }

    buf = alloc_dma_buffer(map_size);
    if (buf == NULL) {
        goto out;
    }
    if (map_dma_buffer(vfio.container_fd, buf, opts.iova, map_size) != 0) {
        goto out;
    }

    printf("srdma DMA buffer ready: userspace=%p iova=0x%" PRIx64
           " requested_size=%zu mapped_size=%zu\n",
           buf, opts.iova, opts.size, map_size);
    dump_prefix("before:", buf, map_size, opts.dump_len);

    if (configure_adminq_and_start_device(&opts, vfio.device_fd, buf,
                                          map_size) != 0) {
        goto unmap;
    }

    dump_prefix("after:", buf, map_size, opts.dump_len);
    ret = 0;

unmap:
    unmap_dma_buffer(vfio.container_fd, opts.iova, map_size);
out:
    if (buf != NULL) {
        munmap(buf, map_size);
    }
    close_vfio(&vfio);
    return ret;
}
