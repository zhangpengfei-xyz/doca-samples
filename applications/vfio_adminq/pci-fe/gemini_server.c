#define _GNU_SOURCE

#include "gemini_server.h"
#include "../common/vfio_adminq_abi.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void close_client(struct gemini_server *server)
{
    if (server->client_fd >= 0)
        close(server->client_fd);
    server->client_fd = -1;
    server->handshaked = false;
}

static int wait_fd(int fd, short events, uint32_t timeout_ms,
                   gemini_progress_cb progress_cb, void *progress_opaque)
{
    uint64_t deadline = monotonic_ms() + timeout_ms;

    for (;;) {
        struct pollfd pfd = {.fd = fd, .events = events};
        uint64_t now = monotonic_ms();
        int wait_ms;
        int rc;

        if (now >= deadline)
            return -ETIMEDOUT;
        wait_ms = (int)(deadline - now);
        if (wait_ms > 50)
            wait_ms = 50;
        rc = poll(&pfd, 1, wait_ms);
        if (progress_cb != NULL)
            progress_cb(progress_opaque);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (rc == 0)
            continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -ECONNRESET;
        if (pfd.revents & events)
            return 0;
    }
}

static int recv_full(int fd, void *buf, size_t len, uint32_t timeout_ms,
                     gemini_progress_cb progress_cb, void *progress_opaque)
{
    uint8_t *pos = buf;
    uint64_t deadline = monotonic_ms() + timeout_ms;

    while (len != 0) {
        ssize_t rc = recv(fd, pos, len, 0);
        uint64_t now;

        if (rc > 0) {
            pos += rc;
            len -= (size_t)rc;
            continue;
        }
        if (rc == 0)
            return -ECONNRESET;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -errno;
        now = monotonic_ms();
        if (now >= deadline)
            return -ETIMEDOUT;
        rc = wait_fd(fd, POLLIN, (uint32_t)(deadline - now),
                     progress_cb, progress_opaque);
        if (rc != 0)
            return (int)rc;
    }
    return 0;
}

static int send_full(int fd, const void *buf, size_t len, uint32_t timeout_ms,
                     gemini_progress_cb progress_cb, void *progress_opaque)
{
    const uint8_t *pos = buf;
    uint64_t deadline = monotonic_ms() + timeout_ms;

    while (len != 0) {
        ssize_t rc = send(fd, pos, len, MSG_NOSIGNAL);
        uint64_t now;

        if (rc > 0) {
            pos += rc;
            len -= (size_t)rc;
            continue;
        }
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return -errno;
        now = monotonic_ms();
        if (now >= deadline)
            return -ETIMEDOUT;
        rc = wait_fd(fd, POLLOUT, (uint32_t)(deadline - now),
                     progress_cb, progress_opaque);
        if (rc != 0)
            return (int)rc;
    }
    return 0;
}

static uint32_t message_payload_len(const struct srdma_gemini_msg *msg)
{
    if ((msg->flags & GEMINI_MSG_F_IS_REPLY) != 0 &&
        (msg->flags & GEMINI_MSG_F_HAS_ERROR) != 0)
        return 0;
    return msg->data_len;
}

static int recv_message(int fd, struct srdma_gemini_msg *msg,
                        uint32_t timeout_ms, gemini_progress_cb progress_cb,
                        void *progress_opaque)
{
    uint32_t len;
    int rc;

    memset(msg, 0, sizeof(*msg));
    rc = recv_full(fd, msg, SRDMA_GEMINI_MSG_HDR_SIZE, timeout_ms,
                   progress_cb, progress_opaque);
    if (rc != 0)
        return rc;
    len = message_payload_len(msg);
    if (len > SRDMA_GEMINI_PAYLOAD_SIZE)
        return -EMSGSIZE;
    if (len == 0)
        return 0;
    return recv_full(fd, &msg->payload, len, timeout_ms,
                     progress_cb, progress_opaque);
}

static int send_message(int fd, struct srdma_gemini_msg *msg,
                        uint32_t timeout_ms, gemini_progress_cb progress_cb,
                        void *progress_opaque)
{
    uint32_t len = message_payload_len(msg);
    int rc;

    if (len > SRDMA_GEMINI_PAYLOAD_SIZE)
        return -EMSGSIZE;
    msg->flags = (msg->flags & ~GEMINI_MSG_F_VERSION_MASK) |
                 GEMINI_MSG_VERSION_CUR;
    rc = send_full(fd, msg, SRDMA_GEMINI_MSG_HDR_SIZE, timeout_ms,
                   progress_cb, progress_opaque);
    if (rc != 0 || len == 0)
        return rc;
    return send_full(fd, &msg->payload, len, timeout_ms,
                     progress_cb, progress_opaque);
}

static int handle_hello(struct gemini_server *server)
{
    struct srdma_gemini_msg request;
    struct srdma_gemini_msg reply;
    int rc;

    rc = recv_message(server->client_fd, &request, 1000, NULL, NULL);
    if (rc != 0)
        return rc;
    if (request.request != GEMINI_HELLO ||
        (request.flags & GEMINI_MSG_F_IS_REPLY) != 0 ||
        (request.flags & GEMINI_MSG_F_VERSION_MASK) != GEMINI_MSG_VERSION_CUR ||
        request.data_len != sizeof(request.payload.hello) ||
        request.payload.hello.type != GEMINI_MSG_TYPE_SRDMA)
        return -EPROTO;

    memset(&reply, 0, sizeof(reply));
    reply.request = GEMINI_HELLO;
    reply.request_id = request.request_id;
    reply.flags = GEMINI_MSG_F_IS_REPLY;
    reply.data_len = sizeof(reply.payload.u8);
    reply.payload.u8 = 0;
    rc = send_message(server->client_fd, &reply, 1000, NULL, NULL);
    if (rc == 0) {
        server->handshaked = true;
        printf("vfio-adminq Gemini SRDMA client connected\n");
    }
    return rc;
}

int gemini_server_init(struct gemini_server *server, const char *path)
{
    struct sockaddr_un addr;

    if (server == NULL || path == NULL || strlen(path) >= sizeof(addr.sun_path))
        return -EINVAL;
    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    server->client_fd = -1;
    server->next_request_id = 1;
    snprintf(server->socket_path, sizeof(server->socket_path), "%s", path);

    server->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server->listen_fd < 0)
        return -errno;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int err = -errno;
        gemini_server_cleanup(server);
        return err;
    }
    if (listen(server->listen_fd, 1) != 0) {
        int err = -errno;
        gemini_server_cleanup(server);
        return err;
    }
    return 0;
}

void gemini_server_cleanup(struct gemini_server *server)
{
    if (server == NULL)
        return;
    close_client(server);
    if (server->listen_fd >= 0)
        close(server->listen_fd);
    server->listen_fd = -1;
    if (server->socket_path[0] != '\0')
        unlink(server->socket_path);
}

void gemini_server_progress(struct gemini_server *server)
{
    struct pollfd pfd;

    if (server == NULL)
        return;
    if (server->client_fd < 0) {
        int fd = accept4(server->listen_fd, NULL, NULL,
                         SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd >= 0) {
            server->client_fd = fd;
            if (handle_hello(server) != 0) {
                fprintf(stderr, "invalid Gemini HELLO; closing client\n");
                close_client(server);
            }
        }
        return;
    }

    pfd.fd = server->client_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) > 0 &&
        (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        fprintf(stderr, "Gemini client disconnected\n");
        close_client(server);
    }
}

bool gemini_server_ready(const struct gemini_server *server)
{
    return server != NULL && server->client_fd >= 0 && server->handshaked;
}

int gemini_server_send_config(struct gemini_server *server,
                              uint8_t subtype,
                              const void *payload,
                              uint32_t payload_len,
                              uint32_t timeout_ms,
                              gemini_progress_cb progress_cb,
                              void *progress_opaque,
                              uint32_t *remote_errcode)
{
    struct srdma_gemini_msg request;
    struct srdma_gemini_msg reply;
    int rc;

    if (!gemini_server_ready(server))
        return -ENOTCONN;
    if (payload_len > SRDMA_GEMINI_PAYLOAD_SIZE ||
        (payload_len != 0 && payload == NULL))
        return -EINVAL;
    if (remote_errcode != NULL)
        *remote_errcode = 0;

    memset(&request, 0, sizeof(request));
    request.request = GEMINI_CONFIG_UPDATE;
    request.request_id = ++server->next_request_id;
    request.flags = ((uint32_t)subtype << GEMINI_MSG_F_CFG_TYPE_SHIFT) |
                    GEMINI_MSG_F_NEEDS_REPLY;
    request.data_len = payload_len;
    if (payload_len != 0)
        memcpy(&request.payload, payload, payload_len);

    rc = send_message(server->client_fd, &request, timeout_ms,
                      progress_cb, progress_opaque);
    if (rc != 0)
        goto disconnect;
    rc = recv_message(server->client_fd, &reply, timeout_ms,
                      progress_cb, progress_opaque);
    if (rc != 0)
        goto disconnect;
    if ((reply.flags & GEMINI_MSG_F_IS_REPLY) == 0 ||
        reply.request != request.request ||
        reply.request_id != request.request_id) {
        rc = -EPROTO;
        goto disconnect;
    }
    if ((reply.flags & GEMINI_MSG_F_HAS_ERROR) != 0) {
        if (remote_errcode != NULL)
            *remote_errcode = reply.errcode;
        return -EREMOTEIO;
    }
    return 0;

disconnect:
    fprintf(stderr, "Gemini request failed: %s\n", strerror(-rc));
    close_client(server);
    return rc;
}
