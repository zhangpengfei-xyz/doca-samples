#include "gemini_client.h"
#include "../common/vfio_adminq_abi.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct srdma_gemini_runtime {
    struct srdma_backend_opts opts;
    bool initialized;
    uint16_t vhca_id;
};

static volatile sig_atomic_t srdma_gemini_stop;

static uint16_t load_le16(const uint8_t value[2])
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint32_t srdma_gemini_payload_len(
    const struct srdma_gemini_msg *msg)
{
    if ((msg->flags & GEMINI_MSG_F_IS_REPLY) &&
        (msg->flags & GEMINI_MSG_F_HAS_ERROR)) {
        return 0;
    }

    return msg->data_len;
}

static void srdma_gemini_signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        srdma_gemini_stop = 1;
    }
}

static void srdma_gemini_install_signals(void)
{
    (void)signal(SIGINT, srdma_gemini_signal_handler);
    (void)signal(SIGTERM, srdma_gemini_signal_handler);
    (void)signal(SIGPIPE, SIG_IGN);
}

static uint64_t monotonic_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec;
}

static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *pos = buf;

    while (len != 0) {
        ssize_t ret = recv(fd, pos, len, 0);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ret == 0) {
            return 1;
        }

        pos += ret;
        len -= (size_t)ret;
    }

    return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *pos = buf;

    while (len != 0) {
        ssize_t ret = send(fd, pos, len, MSG_NOSIGNAL);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        pos += ret;
        len -= (size_t)ret;
    }

    return 0;
}

static int srdma_gemini_connect(const char *socket_path)
{
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Gemini socket path too long: %s\n", socket_path);
        close(fd);
        return -1;
    }
    strcpy(addr.sun_path, socket_path);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "failed to connect Gemini socket %s: %s\n",
                socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int srdma_gemini_send_msg(int fd, struct srdma_gemini_msg *msg)
{
    uint32_t len = srdma_gemini_payload_len(msg);

    msg->flags |= GEMINI_MSG_VERSION_CUR;
    if (len > SRDMA_GEMINI_PAYLOAD_SIZE) {
        fprintf(stderr, "Gemini payload too large to send: %u\n", len);
        return -1;
    }

    if (write_full(fd, msg, SRDMA_GEMINI_MSG_HDR_SIZE) != 0) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    return write_full(fd, &msg->payload, len);
}

static int srdma_gemini_recv_msg(int fd, struct srdma_gemini_msg *msg)
{
    uint32_t len;
    int ret;

    memset(msg, 0, sizeof(*msg));

    ret = read_full(fd, msg, SRDMA_GEMINI_MSG_HDR_SIZE);
    if (ret != 0) {
        return ret;
    }

    len = srdma_gemini_payload_len(msg);
    if (len > SRDMA_GEMINI_PAYLOAD_SIZE) {
        fprintf(stderr, "Gemini payload too large to receive: %u\n", len);
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    return read_full(fd, &msg->payload, len);
}

static int srdma_gemini_send_reply(
    int fd,
    const struct srdma_gemini_msg *request,
    uint8_t errcode)
{
    struct srdma_gemini_msg reply;

    memset(&reply, 0, sizeof(reply));
    reply.request = request->request;
    reply.request_id = request->request_id;
    reply.flags = GEMINI_MSG_F_IS_REPLY;
    if (errcode != 0) {
        reply.flags |= GEMINI_MSG_F_HAS_ERROR;
        reply.errcode = errcode;
    }

    if (srdma_gemini_send_msg(fd, &reply) != 0) {
        fprintf(stderr, "failed to send Gemini reply for request 0x%08x: %s\n",
                request->request_id, strerror(errno));
        return -1;
    }

    return 0;
}

static doca_error_t srdma_gemini_send_hello(int fd)
{
    struct srdma_gemini_msg msg;
    int ret;

    memset(&msg, 0, sizeof(msg));
    msg.request = GEMINI_HELLO;
    msg.request_id = 1;
    msg.flags = GEMINI_MSG_F_NEEDS_REPLY;
    msg.data_len = sizeof(msg.payload.hello);
    msg.payload.hello.type = GEMINI_MSG_TYPE_SRDMA;

    if (srdma_gemini_send_msg(fd, &msg) != 0) {
        fprintf(stderr, "failed to send Gemini HELLO: %s\n", strerror(errno));
        return DOCA_ERROR_IO_FAILED;
    }

    ret = srdma_gemini_recv_msg(fd, &msg);
    if (ret != 0) {
        fprintf(stderr, "failed to receive Gemini HELLO reply\n");
        return DOCA_ERROR_IO_FAILED;
    }
    if (!(msg.flags & GEMINI_MSG_F_IS_REPLY) ||
        msg.request != GEMINI_HELLO ||
        msg.request_id != 1) {
        fprintf(stderr, "invalid Gemini HELLO reply\n");
        return DOCA_ERROR_BAD_STATE;
    }
    if (msg.flags & GEMINI_MSG_F_HAS_ERROR) {
        fprintf(stderr, "Gemini HELLO rejected: errcode=%u\n", msg.errcode);
        return DOCA_ERROR_BAD_STATE;
    }
    if (msg.data_len != sizeof(msg.payload.u8)) {
        fprintf(stderr, "invalid Gemini HELLO reply length: %u\n",
                msg.data_len);
        return DOCA_ERROR_BAD_STATE;
    }

    printf("srdma Gemini client connected: client_id=%u\n", msg.payload.u8);
    return DOCA_SUCCESS;
}

static uint8_t srdma_gemini_handle_plug(
    struct srdma_gemini_runtime *runtime,
    const struct srdma_gemini_msg *msg)
{
    const struct srdma_gemini_srdma_plug_msg *plug =
        &msg->payload.srdma_plug;
    struct srdma_backend_opts opts;
    doca_error_t result;
    uint16_t vhca_id;

    if (msg->data_len != sizeof(*plug)) {
        fprintf(stderr, "invalid SRDMA plug payload length: %u, expect %zu\n",
                msg->data_len, sizeof(*plug));
        return GEMINI_MSG_ERR_INVALID_DATA_LEN;
    }

    vhca_id = load_le16(plug->rsvd0);
    printf("srdma plug message received: rvf_id=%u vhca_id=%u "
           "doorbell_pages=%u max_qp_num=%u netdev_pvf_id=%u\n",
           plug->rvf_id, vhca_id, plug->doorbell_pages, plug->max_qp_num,
           plug->netdev_pvf_id);

    if (runtime->initialized) {
        printf("srdma resources already initialized for vhca_id=%u; "
               "reinitializing\n", runtime->vhca_id);
        srdma_backend_cleanup();
        runtime->initialized = false;
    }

    opts = runtime->opts;
    opts.vhca_id = vhca_id;
    opts.generation = plug->rsvd1[0];

    result = srdma_backend_init(&opts);
    if (result != DOCA_SUCCESS) {
        fprintf(stderr, "failed to initialize SRDMA resources from plug: %s\n",
                doca_error_get_descr(result));
        return GEMINI_MSG_ERR_REQUEST_FAIL;
    }

    runtime->initialized = true;
    runtime->vhca_id = vhca_id;
    printf("srdma resources initialized from plug: rvf_id=%u vhca_id=%u\n",
           plug->rvf_id, vhca_id);
    return 0;
}

static uint8_t srdma_gemini_handle_unplug(
    struct srdma_gemini_runtime *runtime,
    const struct srdma_gemini_msg *msg)
{
    uint16_t vdev_id;

    if (msg->data_len != sizeof(uint16_t)) {
        fprintf(stderr, "invalid SRDMA unplug payload length: %u\n",
                msg->data_len);
        return GEMINI_MSG_ERR_INVALID_DATA_LEN;
    }

    vdev_id = load_le16(msg->payload.raw);
    printf("srdma unplug message received: vdev_id=%u\n", vdev_id);

    if (runtime->initialized) {
        srdma_backend_cleanup();
        runtime->initialized = false;
        printf("srdma resources cleaned up for vdev_id=%u\n", vdev_id);
    }

    return 0;
}

static uint8_t srdma_gemini_handle_start(
    struct srdma_gemini_runtime *runtime,
    const struct srdma_gemini_msg *msg)
{
    const struct srdma_gemini_srdma_start_msg *start =
        &msg->payload.srdma_start;

    if (msg->data_len != sizeof(*start)) {
        fprintf(stderr, "invalid SRDMA start payload length: %u\n",
                msg->data_len);
        return GEMINI_MSG_ERR_INVALID_DATA_LEN;
    }

    printf("srdma start success: rvf_id=%u pcie_port=%u bdf=0x%x "
           "txq=0x%" PRIx64 " rxq=0x%" PRIx64 " aeq=0x%" PRIx64
           " tx_depth=%u rx_depth=%u aeq_depth=%u\n",
           start->rvf_id, start->pcie_port, start->bdf,
           start->txq_addr, start->rxq_addr, start->aeq_addr,
           start->txq_depth, start->rxq_depth, start->aeq_depth);
    if (!runtime->initialized) {
        fprintf(stderr, "SRDMA START received before successful PLUG\n");
        return GEMINI_MSG_ERR_REQUEST_FAIL;
    }
    if (start->txq_addr == 0 || start->txq_depth == 0) {
        fprintf(stderr, "invalid SRDMA START TX queue configuration\n");
        return GEMINI_MSG_ERR_INVALID_PAYLOAD;
    }
    if (start->rxq_addr == 0 || start->aeq_addr == 0 ||
        start->txq_depth != SRDMA_ADMINQ_DEPTH ||
        start->rxq_depth != SRDMA_ADMINQ_DEPTH ||
        start->aeq_depth != SRDMA_AEQ_DEPTH) {
        fprintf(stderr, "invalid SRDMA START queue configuration\n");
        return GEMINI_MSG_ERR_INVALID_PAYLOAD;
    }
    srdma_backend_set_adminq(start->txq_addr, start->rxq_addr,
                             start->aeq_addr, start->txq_depth,
                             start->aeq_depth);
    return 0;
}

static uint8_t srdma_gemini_handle_stop(
    const struct srdma_gemini_msg *msg)
{
    uint16_t vdev_id;

    if (msg->data_len != sizeof(uint16_t)) {
        fprintf(stderr, "invalid SRDMA stop payload length: %u\n",
                msg->data_len);
        return GEMINI_MSG_ERR_INVALID_DATA_LEN;
    }

    vdev_id = load_le16(msg->payload.raw);
    printf("srdma stop message received: vdev_id=%u\n", vdev_id);
    srdma_backend_clear_adminq();
    return 0;
}

static uint8_t srdma_gemini_handle_config_update(
    struct srdma_gemini_runtime *runtime,
    const struct srdma_gemini_msg *msg)
{
    uint8_t cfg_type = GEMINI_MSG_F_CFG_TYPE(msg->flags);

    switch (cfg_type) {
    case GEMINI_MSG_CFG_VDEV_PLUG:
        return srdma_gemini_handle_plug(runtime, msg);
    case GEMINI_MSG_CFG_VDEV_UNPLUG:
        return srdma_gemini_handle_unplug(runtime, msg);
    case GEMINI_MSG_SRDMA_START:
        return srdma_gemini_handle_start(runtime, msg);
    case GEMINI_MSG_SRDMA_STOP:
        return srdma_gemini_handle_stop(msg);
    default:
        fprintf(stderr, "unsupported SRDMA config update type: %u\n",
                cfg_type);
        return GEMINI_MSG_ERR_INVALID_PAYLOAD;
    }
}

static uint8_t srdma_gemini_handle_request(
    struct srdma_gemini_runtime *runtime,
    const struct srdma_gemini_msg *msg)
{
    if (msg->request != GEMINI_CONFIG_UPDATE) {
        fprintf(stderr, "unsupported Gemini request type: %u\n",
                msg->request);
        return GEMINI_MSG_ERR_INVALID_PAYLOAD;
    }

    return srdma_gemini_handle_config_update(runtime, msg);
}

doca_error_t srdma_gemini_serve(
    const char *socket_path,
    const struct srdma_backend_opts *base_opts)
{
    struct srdma_gemini_runtime runtime;
    uint64_t start_sec;
    int fd;
    doca_error_t result;

    if (socket_path == NULL || base_opts == NULL) {
        return DOCA_ERROR_INVALID_VALUE;
    }

    memset(&runtime, 0, sizeof(runtime));
    runtime.opts = *base_opts;
    srdma_gemini_stop = 0;
    srdma_gemini_install_signals();

    fd = srdma_gemini_connect(socket_path);
    if (fd < 0) {
        return DOCA_ERROR_IO_FAILED;
    }

    result = srdma_gemini_send_hello(fd);
    if (result != DOCA_SUCCESS) {
        close(fd);
        return result;
    }

    printf("srdma Gemini client waiting for plug messages on %s\n",
           socket_path);
    start_sec = monotonic_seconds();

    while (!srdma_gemini_stop) {
        struct pollfd pfd = {
            .fd = fd,
            .events = POLLIN,
        };
        int poll_ret;

        if (runtime.initialized) {
            (void)srdma_backend_progress();
        }

        if (runtime.opts.timeout_sec != 0 &&
            monotonic_seconds() - start_sec >= runtime.opts.timeout_sec) {
            break;
        }

        poll_ret = poll(&pfd, 1, 100);
        if (poll_ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            result = DOCA_ERROR_IO_FAILED;
            goto out;
        }
        if (poll_ret == 0) {
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "Gemini socket disconnected: revents=0x%x\n",
                    pfd.revents);
            result = DOCA_ERROR_IO_FAILED;
            goto out;
        }
        if (pfd.revents & POLLIN) {
            struct srdma_gemini_msg msg;
            uint8_t errcode = 0;
            int ret;

            ret = srdma_gemini_recv_msg(fd, &msg);
            if (ret > 0) {
                fprintf(stderr, "Gemini socket closed by peer\n");
                result = DOCA_ERROR_IO_FAILED;
                goto out;
            }
            if (ret < 0) {
                fprintf(stderr, "failed to receive Gemini message: %s\n",
                        strerror(errno));
                result = DOCA_ERROR_IO_FAILED;
                goto out;
            }

            if (!(msg.flags & GEMINI_MSG_F_IS_REPLY)) {
                errcode = srdma_gemini_handle_request(&runtime, &msg);
                if (msg.flags & GEMINI_MSG_F_NEEDS_REPLY) {
                    if (srdma_gemini_send_reply(fd, &msg, errcode) != 0) {
                        result = DOCA_ERROR_IO_FAILED;
                        goto out;
                    }
                }
            }
        }
    }

    result = DOCA_SUCCESS;

out:
    if (runtime.initialized) {
        srdma_backend_cleanup();
    }
    close(fd);
    return result;
}
