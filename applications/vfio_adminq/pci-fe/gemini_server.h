#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gemini_server {
    int listen_fd;
    int client_fd;
    char socket_path[108];
    uint32_t next_request_id;
    bool handshaked;
};

typedef void (*gemini_progress_cb)(void *opaque);

int gemini_server_init(struct gemini_server *server, const char *path);
void gemini_server_cleanup(struct gemini_server *server);
void gemini_server_progress(struct gemini_server *server);
bool gemini_server_ready(const struct gemini_server *server);
int gemini_server_send_config(struct gemini_server *server,
                              uint8_t subtype,
                              const void *payload,
                              uint32_t payload_len,
                              uint32_t timeout_ms,
                              gemini_progress_cb progress_cb,
                              void *progress_opaque,
                              uint32_t *remote_errcode);
