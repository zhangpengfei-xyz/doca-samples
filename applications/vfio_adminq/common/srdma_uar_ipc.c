#define _GNU_SOURCE

#include "srdma_uar_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int build_paths(struct srdma_uar_ipc *ipc, const char *path)
{
    int n;

    n = snprintf(ipc->shm_path, sizeof(ipc->shm_path), "%s.ring", path);
    if (n < 0 || (size_t)n >= sizeof(ipc->shm_path))
        return -ENAMETOOLONG;
    n = snprintf(ipc->socket_path, sizeof(ipc->socket_path), "%s.sock", path);
    if (n < 0 || (size_t)n >= sizeof(ipc->socket_path))
        return -ENAMETOOLONG;
    return 0;
}

static int ensure_parent_dir(const char *path)
{
    char dir[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char *slash;

    if (strlen(path) >= sizeof(dir))
        return -ENAMETOOLONG;
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash == NULL || slash == dir)
        return 0;
    *slash = '\0';
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -errno;
    return 0;
}

static int map_ring(struct srdma_uar_ipc *ipc, bool create)
{
    int flags = create ? O_RDWR | O_CREAT | O_TRUNC : O_RDWR;
    void *mapping;

    ipc->shm_fd = open(ipc->shm_path, flags | O_CLOEXEC, 0600);
    if (ipc->shm_fd < 0)
        return -errno;
    if (create && ftruncate(ipc->shm_fd, sizeof(*ipc->ring)) != 0)
        return -errno;
    mapping = mmap(NULL, sizeof(*ipc->ring), PROT_READ | PROT_WRITE,
                   MAP_SHARED, ipc->shm_fd, 0);
    if (mapping == MAP_FAILED)
        return -errno;
    ipc->ring = mapping;
    if (create) {
        memset(ipc->ring, 0, sizeof(*ipc->ring));
        ipc->ring->magic = SRDMA_UAR_IPC_MAGIC;
        ipc->ring->version = SRDMA_UAR_IPC_VERSION;
        ipc->ring->depth = SRDMA_UAR_IPC_DEPTH;
    } else if (ipc->ring->magic != SRDMA_UAR_IPC_MAGIC ||
               ipc->ring->version != SRDMA_UAR_IPC_VERSION ||
               ipc->ring->depth != SRDMA_UAR_IPC_DEPTH) {
        return -EPROTO;
    }
    return 0;
}

int srdma_uar_ipc_producer_init(struct srdma_uar_ipc *ipc,
                                const char *path)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    int rc;

    memset(ipc, 0, sizeof(*ipc));
    ipc->shm_fd = ipc->socket_fd = ipc->peer_fd = ipc->event_fd = -1;
    ipc->producer = true;
    rc = build_paths(ipc, path);
    if (rc != 0)
        return rc;
    rc = ensure_parent_dir(ipc->shm_path);
    if (rc != 0)
        return rc;
    rc = map_ring(ipc, true);
    if (rc != 0)
        goto fail;
    ipc->socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC |
                            SOCK_NONBLOCK, 0);
    if (ipc->socket_fd < 0) {
        rc = -errno;
        goto fail;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc->socket_path);
    unlink(ipc->socket_path);
    if (bind(ipc->socket_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(ipc->socket_fd, 1) != 0) {
        rc = -errno;
        goto fail;
    }
    return 0;
fail:
    srdma_uar_ipc_cleanup(ipc);
    return rc;
}

int srdma_uar_ipc_producer_progress(struct srdma_uar_ipc *ipc)
{
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    uint8_t byte;
    ssize_t received;

    if (ipc == NULL || !ipc->producer)
        return -EINVAL;
    if (ipc->event_fd >= 0)
        return 0;
    if (ipc->peer_fd < 0) {
        ipc->peer_fd = accept4(ipc->socket_fd, NULL, NULL,
                               SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (ipc->peer_fd < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
    }
    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    iov.iov_base = &byte;
    iov.iov_len = sizeof(byte);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    received = recvmsg(ipc->peer_fd, &msg, 0);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (received != 1) {
        close(ipc->peer_fd);
        ipc->peer_fd = -1;
        return -EPROTO;
    }
    cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(sizeof(int))) {
        close(ipc->peer_fd);
        ipc->peer_fd = -1;
        return -EPROTO;
    }
    memcpy(&ipc->event_fd, CMSG_DATA(cmsg), sizeof(ipc->event_fd));
    return 1;
}

int srdma_uar_ipc_consumer_init(struct srdma_uar_ipc *ipc,
                                const char *path)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    uint8_t byte = 1;
    int rc;

    memset(ipc, 0, sizeof(*ipc));
    ipc->shm_fd = ipc->socket_fd = ipc->peer_fd = ipc->event_fd = -1;
    rc = build_paths(ipc, path);
    if (rc != 0)
        return rc;
    rc = map_ring(ipc, false);
    if (rc != 0)
        goto fail;
    ipc->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ipc->event_fd < 0) {
        rc = -errno;
        goto fail;
    }
    ipc->socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (ipc->socket_fd < 0) {
        rc = -errno;
        goto fail;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc->socket_path);
    if (connect(ipc->socket_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        rc = -errno;
        goto fail;
    }
    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    iov.iov_base = &byte;
    iov.iov_len = sizeof(byte);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &ipc->event_fd, sizeof(ipc->event_fd));
    if (sendmsg(ipc->socket_fd, &msg, MSG_NOSIGNAL) != 1) {
        rc = -errno;
        goto fail;
    }
    return 0;
fail:
    srdma_uar_ipc_cleanup(ipc);
    return rc;
}

int srdma_uar_ipc_push(struct srdma_uar_ipc *ipc,
                       const struct srdma_uar_event *event)
{
    uint32_t producer;
    uint32_t consumer;
    uint64_t one = 1;

    if (ipc == NULL || ipc->ring == NULL || event == NULL || !ipc->producer)
        return -EINVAL;
    producer = atomic_load_explicit(&ipc->ring->producer, memory_order_relaxed);
    consumer = atomic_load_explicit(&ipc->ring->consumer, memory_order_acquire);
    if ((uint32_t)(producer - consumer) >= SRDMA_UAR_IPC_DEPTH) {
        atomic_store_explicit(&ipc->ring->fatal, 1, memory_order_release);
        return -ENOSPC;
    }
    ipc->ring->events[producer % SRDMA_UAR_IPC_DEPTH] = *event;
    atomic_store_explicit(&ipc->ring->producer, producer + 1,
                          memory_order_release);
    if (ipc->event_fd >= 0 && write(ipc->event_fd, &one, sizeof(one)) < 0 &&
        errno != EAGAIN)
        return -errno;
    return 0;
}

int srdma_uar_ipc_pop(struct srdma_uar_ipc *ipc,
                      struct srdma_uar_event *event)
{
    uint32_t producer;
    uint32_t consumer;

    if (ipc == NULL || ipc->ring == NULL || event == NULL || ipc->producer)
        return -EINVAL;
    consumer = atomic_load_explicit(&ipc->ring->consumer, memory_order_relaxed);
    producer = atomic_load_explicit(&ipc->ring->producer, memory_order_acquire);
    if (consumer == producer)
        return 0;
    *event = ipc->ring->events[consumer % SRDMA_UAR_IPC_DEPTH];
    atomic_store_explicit(&ipc->ring->consumer, consumer + 1,
                          memory_order_release);
    return 1;
}

int srdma_uar_ipc_event_fd(const struct srdma_uar_ipc *ipc)
{
    return ipc == NULL ? -1 : ipc->event_fd;
}

void srdma_uar_ipc_mark_fatal(struct srdma_uar_ipc *ipc)
{
    if (ipc != NULL && ipc->ring != NULL)
        atomic_store_explicit(&ipc->ring->fatal, 1, memory_order_release);
}

bool srdma_uar_ipc_is_fatal(const struct srdma_uar_ipc *ipc)
{
    return ipc != NULL && ipc->ring != NULL &&
           atomic_load_explicit(&ipc->ring->fatal, memory_order_acquire) != 0;
}

void srdma_uar_ipc_cleanup(struct srdma_uar_ipc *ipc)
{
    if (ipc == NULL)
        return;
    if (ipc->ring != NULL)
        munmap(ipc->ring, sizeof(*ipc->ring));
    if (ipc->event_fd >= 0)
        close(ipc->event_fd);
    if (ipc->peer_fd >= 0)
        close(ipc->peer_fd);
    if (ipc->socket_fd >= 0)
        close(ipc->socket_fd);
    if (ipc->shm_fd >= 0)
        close(ipc->shm_fd);
    if (ipc->producer) {
        if (ipc->socket_path[0] != '\0')
            unlink(ipc->socket_path);
        if (ipc->shm_path[0] != '\0')
            unlink(ipc->shm_path);
    }
    memset(ipc, 0, sizeof(*ipc));
    ipc->shm_fd = ipc->socket_fd = ipc->peer_fd = ipc->event_fd = -1;
}
