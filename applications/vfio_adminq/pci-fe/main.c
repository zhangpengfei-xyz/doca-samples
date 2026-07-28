#define _GNU_SOURCE

#include "pci_fe.h"
#include "gemini_server.h"
#include "../common/vfio_adminq_abi.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_log.h>

#define DEFAULT_CONTROL_SOCKET "/run/vfio-adminq/pci-fe.sock"

static volatile sig_atomic_t stop_requested;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
        stop_requested = 1;
}

static void usage(const char *prog)
{
    printf("Usage:\n"
           "  %s serve [--pci-addr <addr>] [--gemini-socket <path>] "
           "--netdev-mac <xx:xx:xx:xx:xx:xx> "
           "[--control-socket <path>]\n"
           "  %s plug|unplug|status [--control-socket <path>]\n",
           prog, prog);
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

static int control_server_init(const char *path)
{
    struct sockaddr_un addr;
    int fd;
    int rc;

    rc = ensure_parent_dir(path);
    if (rc != 0)
        return rc;
    if (strlen(path) >= sizeof(addr.sun_path))
        return -ENAMETOOLONG;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -errno;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        rc = -errno;
        close(fd);
        return rc;
    }
    if (listen(fd, 4) != 0) {
        rc = -errno;
        close(fd);
        unlink(path);
        return rc;
    }
    return fd;
}

static void control_server_progress(int listen_fd, struct pci_fe *fe)
{
    char command[32];
    char reply[160];
    int client;
    ssize_t len;
    int rc;

    client = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0)
        return;
    len = recv(client, command, sizeof(command) - 1, 0);
    if (len <= 0) {
        close(client);
        return;
    }
    command[len] = '\0';
    command[strcspn(command, "\r\n \t")] = '\0';

    if (strcmp(command, "plug") == 0) {
        rc = pci_fe_plug(fe);
        snprintf(reply, sizeof(reply), "%s rc=%d state=%s vhca_id=%u\n",
                 rc == 0 ? "OK" : "ERROR", rc,
                 pci_fe_state_name(fe->state), fe->vhca_id);
    } else if (strcmp(command, "unplug") == 0) {
        rc = pci_fe_unplug(fe, true);
        snprintf(reply, sizeof(reply), "%s rc=%d state=%s\n",
                 rc == 0 ? "OK" : "ERROR", rc,
                 pci_fe_state_name(fe->state));
    } else if (strcmp(command, "status") == 0) {
        snprintf(reply, sizeof(reply),
                 "OK state=%s gemini=%s vhca_id=%u bdf=0x%04x ready=%u started=%u\n",
                 pci_fe_state_name(fe->state),
                 gemini_server_ready(fe->gemini) ? "connected" : "disconnected",
                 fe->vhca_id, fe->bdf, fe->ready, fe->init_done);
    } else {
        snprintf(reply, sizeof(reply), "ERROR rc=%d unknown-command\n", -EINVAL);
    }
    (void)send(client, reply, strlen(reply), MSG_NOSIGNAL);
    close(client);
}

static int send_control_command(const char *path, const char *command)
{
    struct sockaddr_un addr;
    char reply[256];
    ssize_t len;
    int fd;

    if (strlen(path) >= sizeof(addr.sun_path))
        return 2;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect control socket");
        close(fd);
        return 1;
    }
    if (send(fd, command, strlen(command), MSG_NOSIGNAL) < 0) {
        perror("send control command");
        close(fd);
        return 1;
    }
    len = recv(fd, reply, sizeof(reply) - 1, 0);
    if (len < 0) {
        perror("receive control reply");
        close(fd);
        return 1;
    }
    reply[len] = '\0';
    fputs(reply, stdout);
    close(fd);
    return strncmp(reply, "OK", 2) == 0 ? 0 : 1;
}

static int parse_mac(const char *text, uint8_t mac[6])
{
    unsigned int b[6];
    int consumed = 0;

    if (text == NULL || sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%n",
                               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5],
                               &consumed) != 6 || text[consumed] != '\0')
        return -EINVAL;
    for (unsigned int i = 0; i < 6; i++)
        mac[i] = b[i];
    return 0;
}

static int serve(const char *pci_addr, const char *gemini_path,
                 const char *control_path, const uint8_t mac[6])
{
    struct gemini_server gemini;
    struct pci_fe fe;
    doca_error_t result;
    int control_fd = -1;
    int rc;

    rc = ensure_parent_dir(gemini_path);
    if (rc != 0) {
        fprintf(stderr, "failed to create Gemini socket directory: %s\n",
                strerror(-rc));
        return 1;
    }
    rc = gemini_server_init(&gemini, gemini_path);
    if (rc != 0) {
        fprintf(stderr, "failed to create Gemini server: %s\n", strerror(-rc));
        return 1;
    }
    result = pci_fe_init(&fe, pci_addr, &gemini, mac);
    if (result != DOCA_SUCCESS) {
        fprintf(stderr, "failed to initialize pci-fe: %s\n",
                doca_error_get_descr(result));
        gemini_server_cleanup(&gemini);
        return 1;
    }
    control_fd = control_server_init(control_path);
    if (control_fd < 0) {
        fprintf(stderr, "failed to create control server: %s\n",
                strerror(-control_fd));
        pci_fe_cleanup(&fe);
        gemini_server_cleanup(&gemini);
        return 1;
    }

    printf("vfio-adminq pci-fe serving: pci=%s gemini=%s control=%s\n",
           pci_addr, gemini_path, control_path);
    while (!stop_requested) {
        struct pollfd pfd = {.fd = control_fd, .events = POLLIN};

        gemini_server_progress(&gemini);
        pci_fe_progress(&fe);
        pci_fe_process_pending(&fe);
        if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN) != 0)
            control_server_progress(control_fd, &fe);
    }

    pci_fe_cleanup(&fe);
    gemini_server_cleanup(&gemini);
    close(control_fd);
    unlink(control_path);
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"pci-addr", required_argument, NULL, 'p'},
        {"gemini-socket", required_argument, NULL, 'g'},
        {"control-socket", required_argument, NULL, 'c'},
        {"netdev-mac", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *pci_addr = VFIO_ADMINQ_DEFAULT_DOCA_PCI_ADDR;
    const char *gemini_path = SRDMA_GEMINI_DEFAULT_SOCKET;
    const char *control_path = DEFAULT_CONTROL_SOCKET;
    const char *mac_text = NULL;
    uint8_t mac[6];
    const char *command;
    int opt;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    command = argv[1];
    optind = 2;
    while ((opt = getopt_long(argc, argv, "p:g:c:m:h", options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            pci_addr = optarg;
            break;
        case 'g':
            gemini_path = optarg;
            break;
        case 'c':
            control_path = optarg;
            break;
        case 'm':
            mac_text = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (optind != argc) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(command, "plug") == 0 || strcmp(command, "unplug") == 0 ||
        strcmp(command, "status") == 0)
        return send_control_command(control_path, command);
    if (strcmp(command, "serve") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (parse_mac(mac_text, mac) != 0) {
        fprintf(stderr, "serve requires a valid --netdev-mac\n");
        return 2;
    }

    (void)doca_log_backend_create_standard();
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    return serve(pci_addr, gemini_path, control_path, mac);
}
