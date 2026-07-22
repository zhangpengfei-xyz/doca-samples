#pragma once

#include "srdma_backend.h"
#include "../common/vfio_adminq_abi.h"

doca_error_t srdma_gemini_serve(
    const char *socket_path,
    const struct srdma_backend_opts *base_opts);
