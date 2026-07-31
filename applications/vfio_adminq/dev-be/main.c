#include "gemini_client.h"
#include "srdma_backend.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <doca_error.h>

static void print_usage(const char *prog)
{
    printf("Usage: %s serve [options]\n"
           "\n"
           "Options:\n"
           "  --pci-addr <addr>       DOCA device PCI address "
           "(default: %s)\n"
           "  --type-name <name>      DOCA PCI TLP type name "
           "(default: %s)\n"
           "  --num-db <count>        endpoint doorbell count "
           "(default: %u)\n"
           "  --local-dma-size <n>    local DMA mmap size "
           "(default: %u)\n"
           "  --socket <path>         Gemini server socket "
           "(default: %s)\n"
           "  --timeout-sec <n>       serve timeout; 0 means forever "
           "(default: %u)\n"
           "  --dma-timeout-ms <n>    individual DMA timeout "
           "(default: %u)\n"
           "  -h, --help              show this help\n",
           prog, SRDMA_DPU_DEFAULT_PCI_ADDR,
           SRDMA_DPU_DEFAULT_PCI_TYPE_NAME,
           SRDMA_DPU_DEFAULT_DB_COUNT, SRDMA_DPU_DEFAULT_LOCAL_DMA_SIZE,
           SRDMA_GEMINI_DEFAULT_SOCKET, 0,
           SRDMA_DPU_DEFAULT_DMA_TIMEOUT_MS);
}

static int parse_u16(const char *name, const char *value, uint16_t *out)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT16_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        return -1;
    }

    *out = (uint16_t)parsed;
    return 0;
}

static int parse_u32(const char *name, const char *value, uint32_t *out)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        return -1;
    }

    *out = (uint32_t)parsed;
    return 0;
}

static int parse_size(const char *name, const char *value, size_t *out)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        return -1;
    }

    *out = (size_t)parsed;
    return 0;
}

static int parse_args(int argc, char **argv,
                      struct srdma_backend_opts *opts,
                      const char **gemini_socket)
{
    static const struct option long_options[] = {
        {"pci-addr", required_argument, NULL, 'p'},
        {"type-name", required_argument, NULL, 't'},
        {"num-db", required_argument, NULL, 'n'},
        {"local-dma-size", required_argument, NULL, 's'},
        {"socket", required_argument, NULL, 'g'},
        {"timeout-sec", required_argument, NULL, 'T'},
        {"dma-timeout-ms", required_argument, NULL, 'D'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int opt;

    while ((opt = getopt_long(argc, argv, "h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            opts->pci_addr = optarg;
            break;
        case 't':
            opts->pci_type_name = optarg;
            break;
        case 'n':
            if (parse_u16("num-db", optarg, &opts->num_db) != 0) {
                return -1;
            }
            break;
        case 's':
            if (parse_size("local-dma-size", optarg,
                           &opts->local_dma_size) != 0) {
                return -1;
            }
            break;
        case 'g':
            *gemini_socket = optarg;
            break;
        case 'T':
            if (parse_u32("timeout-sec", optarg, &opts->timeout_sec) != 0) {
                return -1;
            }
            break;
        case 'D':
            if (parse_u32("dma-timeout-ms", optarg,
                          &opts->dma_timeout_ms) != 0 ||
                opts->dma_timeout_ms == 0) {
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

int main(int argc, char **argv)
{
    struct srdma_backend_opts opts;
    const char *gemini_socket = SRDMA_GEMINI_DEFAULT_SOCKET;
    doca_error_t result;
    int parse_result;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2 || strcmp(argv[1], "serve") != 0) {
        print_usage(argv[0]);
        return argc < 2 ? 1 : 2;
    }

    srdma_backend_default_opts(&opts);
    optind = 2;
    parse_result = parse_args(argc, argv, &opts, &gemini_socket);
    if (parse_result > 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (parse_result < 0) {
        print_usage(argv[0]);
        return 2;
    }

    result = srdma_gemini_serve(gemini_socket, &opts);
    if (result != DOCA_SUCCESS) {
        fprintf(stderr, "srdma backend serve failed: %s\n",
                doca_error_get_descr(result));
        return 1;
    }

    printf("srdma backend serve completed\n");
    return 0;
}
