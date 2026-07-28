// SPDX-License-Identifier: BSD-2-Clause
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <infiniband/verbs.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DEFAULT_MR_SIZE (64U * 1024U)
#define HEALTH_IDLE_US (1700U * 1000U)
#define POLL_COUNT 40
#define POLL_INTERVAL_US (50U * 1000U)

struct options {
	const char *dev_name;
	const char *gid_cidr;
	unsigned int port;
	unsigned int vector;
	size_t mr_size;
	bool basic_qp;
};

struct command_count {
	const char *name;
	uint64_t n;
	uint64_t failed;
	bool valid;
};

struct command_snapshot {
	struct command_count command[30];
};

static const char *const command_names[] = {
	"GET_CAP", "GET_CAP_EXT", "HEALTH_CHECK", "ADD_GID", "DEL_GID",
	"CREATE_EQ", "DESTROY_EQ", "ALLOC_PD", "DEALLOC_PD", "REG_MR",
	"DEREG_MR", "CREATE_CQ", "DESTROY_CQ", "CREATE_QP", "DESTROY_QP",
	"QUERY_QP", "ALLOC_UCTX", "DEALLOC_UCTX", "GET_STATS",
	"GET_STATS_EXT", "GET_STATS_EXT2", "GET_STATS_EXT3", "RST2INIT_QP",
	"INIT2INIT_QP", "INIT2RTR_QP", "RTR2RTS_QP", "RTS2RTS_QP",
	"2ERR_QP", "2RST_QP",
};

static volatile sig_atomic_t stop_requested;
static unsigned int passes;
static unsigned int failures;
static unsigned int warnings;

static void on_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static void result(const char *kind, const char *name, const char *detail)
{
	printf("[%s] %-24s %s\n", kind, name, detail ? detail : "");
}

static void pass(const char *name, const char *detail)
{
	passes++;
	result("PASS", name, detail);
}

static void fail(const char *name, const char *detail)
{
	failures++;
	result("FAIL", name, detail);
}

static void warn(const char *name, const char *detail)
{
	warnings++;
	result("WARN", name, detail);
}

static void usage(const char *program)
{
	printf("Usage: %s [options]\n"
	       "  -d, --device NAME       RDMA device (default: first srdma*)\n"
	       "  -p, --port N            RDMA port (default: 1)\n"
	       "  -v, --vector N          CQ completion vector (default: 1)\n"
	       "  -m, --mr-size BYTES     registered MR size (default: 65536)\n"
	       "  -g, --gid-cycle CIDR    temporarily add/delete CIDR on bound netdev\n"
	       "      --basic-qp          stop QP at INIT; skip RTR/RTS/ERR/RESET\n"
	       "  -h, --help              show this help\n",
	       program);
}

static int parse_u32(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno || !*text || *end || parsed > UINT32_MAX)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

static int parse_size(const char *text, size_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno || !*text || *end || !parsed || parsed > SIZE_MAX)
		return -1;
	*value = (size_t)parsed;
	return 0;
}

static int parse_options(int argc, char **argv, struct options *opts)
{
	static const struct option long_options[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "port", required_argument, NULL, 'p' },
		{ "vector", required_argument, NULL, 'v' },
		{ "mr-size", required_argument, NULL, 'm' },
		{ "gid-cycle", required_argument, NULL, 'g' },
		{ "basic-qp", no_argument, NULL, 1 },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int option;

	while ((option = getopt_long(argc, argv, "d:p:v:m:g:h", long_options,
				     NULL)) != -1) {
		switch (option) {
		case 'd':
			opts->dev_name = optarg;
			break;
		case 'p':
			if (parse_u32(optarg, &opts->port) || !opts->port)
				return -1;
			break;
		case 'v':
			if (parse_u32(optarg, &opts->vector))
				return -1;
			break;
		case 'm':
			if (parse_size(optarg, &opts->mr_size))
				return -1;
			break;
		case 'g':
			opts->gid_cidr = optarg;
			break;
		case 1:
			opts->basic_qp = true;
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		default:
			return -1;
		}
	}

	return optind == argc ? 0 : -1;
}

static int read_text(const char *path, char *buf, size_t len)
{
	FILE *file;
	size_t nread;

	file = fopen(path, "r");
	if (!file)
		return -1;
	nread = fread(buf, 1, len - 1, file);
	if (ferror(file)) {
		fclose(file);
		return -1;
	}
	buf[nread] = '\0';
	fclose(file);
	while (nread && (buf[nread - 1] == '\n' || buf[nread - 1] == '\r'))
		buf[--nread] = '\0';
	return 0;
}

static int read_u64(const char *path, uint64_t *value)
{
	char buf[64];
	char *end;
	unsigned long long parsed;

	if (read_text(path, buf, sizeof(buf)))
		return -1;
	errno = 0;
	parsed = strtoull(buf, &end, 0);
	if (errno || end == buf || (*end && *end != '\n'))
		return -1;
	*value = parsed;
	return 0;
}

static int join_path(char *out, size_t out_len, const char *directory,
		     const char *name)
{
	size_t directory_len = strlen(directory);
	size_t name_len = strlen(name);
	bool needs_slash = directory_len && directory[directory_len - 1] != '/';

	if (directory_len + needs_slash + name_len + 1 > out_len)
		return -1;
	memcpy(out, directory, directory_len);
	if (needs_slash)
		out[directory_len++] = '/';
	memcpy(out + directory_len, name, name_len + 1);
	return 0;
}

static int discover_bdf(const char *dev_name, char *bdf, size_t bdf_len)
{
	char path[PATH_MAX];
	char resolved[PATH_MAX];
	const char *base;

	snprintf(path, sizeof(path), "/sys/class/infiniband/%s/device", dev_name);
	if (!realpath(path, resolved))
		return -1;
	base = strrchr(resolved, '/');
	base = base ? base + 1 : resolved;
	if (snprintf(bdf, bdf_len, "%s", base) >= (int)bdf_len)
		return -1;
	return 0;
}

static int discover_netdev(const char *dev_name, char *netdev, size_t len)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *entry;
	int rc = -1;

	snprintf(path, sizeof(path), "/sys/class/infiniband/%s/device/net", dev_name);
	dir = opendir(path);
	if (!dir)
		return -1;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.')
			continue;
		if (strlen(entry->d_name) < len) {
			memcpy(netdev, entry->d_name, strlen(entry->d_name) + 1);
			rc = 0;
		}
		break;
	}
	closedir(dir);
	return rc;
}

static int discover_gid_netdev(const char *dev_name, unsigned int port,
			       unsigned int gid_table_len, char *netdev, size_t len)
{
	unsigned int i;

	for (i = 0; i < gid_table_len; i++) {
		char path[PATH_MAX];
		char value[IF_NAMESIZE];

		if (snprintf(path, sizeof(path),
			     "/sys/class/infiniband/%s/ports/%u/gid_attrs/ndevs/%u",
			     dev_name, port, i) >= (int)sizeof(path))
			return -1;
		if (read_text(path, value, sizeof(value)) || !value[0])
			continue;
		if (strlen(value) >= len)
			return -1;
		memcpy(netdev, value, strlen(value) + 1);
		return 0;
	}
	return -1;
}

static int snapshot_commands(const char *debug_root,
			     struct command_snapshot *snapshot)
{
	size_t i;
	int valid = 0;

	memset(snapshot, 0, sizeof(*snapshot));
	for (i = 0; i < ARRAY_SIZE(command_names); i++) {
		char path[PATH_MAX];
		struct command_count *count = &snapshot->command[i];

		count->name = command_names[i];
		snprintf(path, sizeof(path), "%s/commands/%s/n", debug_root,
			 command_names[i]);
		if (read_u64(path, &count->n))
			continue;
		snprintf(path, sizeof(path), "%s/commands/%s/failed", debug_root,
			 command_names[i]);
		if (read_u64(path, &count->failed))
			continue;
		count->valid = true;
		valid++;
	}
	return valid == (int)ARRAY_SIZE(command_names) ? 0 : -1;
}

static const struct command_count *find_command(const struct command_snapshot *snapshot,
						const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(command_names); i++)
		if (!strcmp(snapshot->command[i].name, name))
			return &snapshot->command[i];
	return NULL;
}

static uint64_t command_delta(const struct command_snapshot *before,
			      const struct command_snapshot *after,
			      const char *name)
{
	const struct command_count *old = find_command(before, name);
	const struct command_count *new = find_command(after, name);

	if (!old || !new || !old->valid || !new->valid || new->n < old->n)
		return 0;
	return new->n - old->n;
}

static void expect_command(const struct command_snapshot *before,
			   const struct command_snapshot *after,
			   const char *name, bool allow_existing)
{
	const struct command_count *old = find_command(before, name);
	const struct command_count *new = find_command(after, name);
	char detail[128];

	if (!old || !new || !old->valid || !new->valid) {
		fail(name, "debugfs counter unavailable");
		return;
	}
	if (new->failed != old->failed) {
		snprintf(detail, sizeof(detail), "failed counter increased: %" PRIu64
			 " -> %" PRIu64, old->failed, new->failed);
		fail(name, detail);
		return;
	}
	if (new->n > old->n) {
		snprintf(detail, sizeof(detail), "AdminQ count +%" PRIu64,
			 new->n - old->n);
		pass(name, detail);
		return;
	}
	if (allow_existing && new->n) {
		snprintf(detail, sizeof(detail), "already active, total=%" PRIu64, new->n);
		pass(name, detail);
		return;
	}
	fail(name, "AdminQ count did not increase");
}

static int count_subdirs(const char *path)
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;

	dir = opendir(path);
	if (!dir)
		return -1;
	while ((entry = readdir(dir)))
		if (entry->d_name[0] != '.')
			count++;
	closedir(dir);
	return count;
}

static int run_command(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		execvp(argv[0], argv);
		perror(argv[0]);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool gid_is_zero(const union ibv_gid *gid)
{
	static const uint8_t zero[16];

	return !memcmp(gid->raw, zero, sizeof(zero));
}

static int enumerate_gids(struct ibv_context *ctx, unsigned int port,
			  unsigned int table_len, union ibv_gid *first,
			  unsigned int *first_index)
{
	unsigned int i;
	int found = 0;

	printf("GID table:\n");
	for (i = 0; i < table_len; i++) {
		union ibv_gid gid;
		char text[INET6_ADDRSTRLEN];

		if (ibv_query_gid(ctx, port, i, &gid))
			continue;
		if (gid_is_zero(&gid))
			continue;
		if (!inet_ntop(AF_INET6, gid.raw, text, sizeof(text)))
			snprintf(text, sizeof(text), "<invalid>");
		printf("  [%u] %s\n", i, text);
		if (!found) {
			*first = gid;
			*first_index = i;
		}
		found++;
	}
	return found;
}

static int wait_command_delta(const char *debug_root,
			      const struct command_snapshot *before,
			      const char *name)
{
	int i;

	for (i = 0; i < POLL_COUNT && !stop_requested; i++) {
		struct command_snapshot now;

		if (!snapshot_commands(debug_root, &now) &&
		    command_delta(before, &now, name))
			return 0;
		usleep(POLL_INTERVAL_US);
	}
	return -1;
}

static int test_gid_cycle(const char *debug_root, const char *netdev,
			  const char *cidr)
{
	struct command_snapshot before_add;
	struct command_snapshot after_add;
	struct command_snapshot before_del;
	struct command_snapshot after_del;
	char *add_argv[] = { "ip", "address", "add", (char *)cidr,
			     "dev", (char *)netdev, NULL };
	char *del_argv[] = { "ip", "address", "del", (char *)cidr,
			     "dev", (char *)netdev, NULL };
	int rc = -1;

	if (snapshot_commands(debug_root, &before_add)) {
		fail("GID ADD/DEL", "cannot snapshot AdminQ counters");
		return -1;
	}
	printf("Temporarily adding %s to %s\n", cidr, netdev);
	if (run_command(add_argv)) {
		fail("ADD_GID", "ip address add failed; address must be unused");
		return -1;
	}
	if (wait_command_delta(debug_root, &before_add, "ADD_GID") ||
	    snapshot_commands(debug_root, &after_add)) {
		fail("ADD_GID", "AdminQ counter did not increase");
	} else {
		expect_command(&before_add, &after_add, "ADD_GID", false);
	}

	if (snapshot_commands(debug_root, &before_del))
		goto delete_address;
	rc = 0;

delete_address:
	if (run_command(del_argv)) {
		fail("DEL_GID", "failed to remove temporary address");
		return -1;
	}
	if (rc || wait_command_delta(debug_root, &before_del, "DEL_GID") ||
	    snapshot_commands(debug_root, &after_del)) {
		fail("DEL_GID", "AdminQ counter did not increase");
		return -1;
	}
	expect_command(&before_del, &after_del, "DEL_GID", false);
	return 0;
}

static int read_hw_stats(const char *dev_name, unsigned int port)
{
	static const char *const names[] = {
		"tx_pkts", "cq_overflow", "mac0_link_status", "global_cnp_sent",
	};
	char directory[PATH_MAX];
	unsigned int i;
	int read_count = 0;

	snprintf(directory, sizeof(directory),
		 "/sys/class/infiniband/%s/ports/%u/hw_counters", dev_name, port);
	printf("Representative hardware counters:\n");
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		char path[PATH_MAX];
		uint64_t value;

		if (join_path(path, sizeof(path), directory, names[i]))
			continue;
		if (read_u64(path, &value)) {
			printf("  %-24s unavailable\n", names[i]);
			continue;
		}
		printf("  %-24s %" PRIu64 "\n", names[i], value);
		read_count++;
	}
	return read_count == (int)ARRAY_SIZE(names) ? 0 : -1;
}

static int query_qp_state(struct ibv_qp *qp, enum ibv_qp_state expected)
{
	struct ibv_qp_attr attr = { 0 };
	struct ibv_qp_init_attr init = { 0 };
	char detail[80];

	if (ibv_query_qp(qp, &attr, IBV_QP_STATE, &init)) {
		fail("QP query", strerror(errno));
		return -1;
	}
	snprintf(detail, sizeof(detail), "QPN=%u state=%d", qp->qp_num,
		 (int)attr.qp_state);
	if (attr.qp_state != expected) {
		fail("QP query", detail);
		return -1;
	}
	pass("QP query", detail);
	return 0;
}

static int qp_to_init(struct ibv_qp *qp, unsigned int port)
{
	struct ibv_qp_attr attr = { 0 };
	int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
		   IBV_QP_ACCESS_FLAGS;

	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = port;
	attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	if (ibv_modify_qp(qp, &attr, mask)) {
		fail("QP RESET -> INIT", strerror(errno));
		return -1;
	}
	pass("QP RESET -> INIT", "no WQE posted");
	return query_qp_state(qp, IBV_QPS_INIT);
}

static int qp_to_rts(struct ibv_qp *qp, unsigned int port,
		     enum ibv_mtu mtu, const union ibv_gid *gid,
		     unsigned int gid_index)
{
	struct ibv_qp_attr attr = { 0 };
	int mask;

	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = mtu;
	attr.dest_qp_num = qp->qp_num;
	attr.rq_psn = 0;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 1;
	attr.ah_attr.port_num = port;
	attr.ah_attr.grh.dgid = *gid;
	attr.ah_attr.grh.sgid_index = gid_index;
	attr.ah_attr.grh.hop_limit = 1;
	mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
	       IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
	if (ibv_modify_qp(qp, &attr, mask)) {
		fail("QP INIT -> RTR", strerror(errno));
		return -1;
	}
	pass("QP INIT -> RTR", "self path configured; no WQE posted");
	if (query_qp_state(qp, IBV_QPS_RTR))
		return -1;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = 0;
	attr.max_rd_atomic = 1;
	mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
	       IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
	if (ibv_modify_qp(qp, &attr, mask)) {
		fail("QP RTR -> RTS", strerror(errno));
		return -1;
	}
	pass("QP RTR -> RTS", "state only; no WQE posted");
	if (query_qp_state(qp, IBV_QPS_RTS))
		return -1;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_ERR;
	if (ibv_modify_qp(qp, &attr, IBV_QP_STATE)) {
		fail("QP RTS -> ERR", strerror(errno));
		return -1;
	}
	pass("QP RTS -> ERR", "state transition accepted");

	attr.qp_state = IBV_QPS_RESET;
	if (ibv_modify_qp(qp, &attr, IBV_QP_STATE)) {
		fail("QP ERR -> RESET", strerror(errno));
		return -1;
	}
	pass("QP ERR -> RESET", "state transition accepted");
	return query_qp_state(qp, IBV_QPS_RESET);
}

static int test_debug_objects(const char *debug_root)
{
	static const struct {
		const char *name;
		int minimum;
	} objects[] = {
		{ "eqs", 2 }, { "cqs", 1 }, { "qps", 1 }, { "mrs", 1 },
	};
	char state_path[PATH_MAX];
	char state[64];
	unsigned int i;
	int rc = 0;

	if (join_path(state_path, sizeof(state_path), debug_root, "device/state")) {
		fail("driver state", "debugfs path is too long");
		return -1;
	}
	if (read_text(state_path, state, sizeof(state)) || strcmp(state, "running")) {
		fail("driver state", "debugfs state is not running");
		rc = -1;
	} else {
		pass("driver state", "running");
	}
	for (i = 0; i < ARRAY_SIZE(objects); i++) {
		char path[PATH_MAX];
		char detail[80];
		int count;

		if (join_path(path, sizeof(path), debug_root, objects[i].name)) {
			fail(objects[i].name, "debugfs path is too long");
			rc = -1;
			continue;
		}
		count = count_subdirs(path);
		snprintf(detail, sizeof(detail), "%d live object(s)", count);
		if (count < objects[i].minimum) {
			fail(objects[i].name, detail);
			rc = -1;
		} else {
			pass(objects[i].name, detail);
		}
	}
	return rc;
}

static int test_devlink_health(const char *bdf)
{
	char devlink_name[64];
	char *show_argv[] = { "devlink", "health", "show", devlink_name,
			      "reporter", "fw", NULL };
	char *diagnose_argv[] = { "devlink", "health", "diagnose", devlink_name,
				  "reporter", "fw", NULL };

	snprintf(devlink_name, sizeof(devlink_name), "pci/%s", bdf);
	printf("Devlink health reporter:\n");
	if (run_command(show_argv) || run_command(diagnose_argv)) {
		fail("devlink health", "fw reporter show/diagnose failed");
		return -1;
	}
	pass("devlink health", "fw reporter show and diagnose succeeded");
	return 0;
}

static struct ibv_device *select_device(struct ibv_device **list, int count,
					const char *requested)
{
	int i;

	for (i = 0; i < count; i++) {
		const char *name = ibv_get_device_name(list[i]);

		if ((requested && !strcmp(name, requested)) ||
		    (!requested && !strncmp(name, "srdma", 5)))
			return list[i];
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct options opts = {
		.port = 1,
		.vector = 1,
		.mr_size = DEFAULT_MR_SIZE,
	};
	struct command_snapshot before = { 0 };
	struct command_snapshot after = { 0 };
	struct ibv_device **device_list = NULL;
	struct ibv_device *device;
	struct ibv_context *ctx = NULL;
	struct ibv_device_attr device_attr = { 0 };
	struct ibv_port_attr port_attr = { 0 };
	struct ibv_pd *pd = NULL;
	struct ibv_mr *mr = NULL;
	struct ibv_cq *cq = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_qp_init_attr qp_init = { 0 };
	union ibv_gid first_gid = { 0 };
	unsigned int first_gid_index = 0;
	char dev_name[64];
	char bdf[32];
	char netdev[IF_NAMESIZE] = { 0 };
	char debug_root[PATH_MAX];
	char detail[256];
	void *buffer = NULL;
	int num_devices = 0;
	int gid_count = 0;
	bool debug_ok = false;
	bool opened = false;
	bool pd_created = false;
	bool mr_created = false;
	bool cq_created = false;
	bool qp_created = false;
	bool qp_init_done = false;
	bool qp_full_done = false;
	bool stats_done = false;
	bool health_done = false;

	if (parse_options(argc, argv, &opts)) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (geteuid()) {
		fprintf(stderr, "This test must run as root (debugfs/devlink access).\n");
		return EXIT_FAILURE;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	device_list = ibv_get_device_list(&num_devices);
	if (!device_list) {
		perror("ibv_get_device_list");
		return EXIT_FAILURE;
	}
	device = select_device(device_list, num_devices, opts.dev_name);
	if (!device) {
		fprintf(stderr, "No matching sRDMA device. Is srdma.ko and its provider loaded?\n");
		ibv_free_device_list(device_list);
		return EXIT_FAILURE;
	}
	snprintf(dev_name, sizeof(dev_name), "%s", ibv_get_device_name(device));
	if (discover_bdf(dev_name, bdf, sizeof(bdf))) {
		fprintf(stderr, "Cannot resolve PCI BDF for %s\n", dev_name);
		goto cleanup;
	}
	discover_netdev(dev_name, netdev, sizeof(netdev));
	snprintf(debug_root, sizeof(debug_root), "/sys/kernel/debug/srdma/%s", bdf);
	printf("sRDMA control-plane test\n"
	       "  device=%s bdf=%s netdev=%s port=%u vector=%u\n"
	       "  data-plane WQEs: disabled by design\n",
	       dev_name, bdf, netdev[0] ? netdev : "<none>", opts.port, opts.vector);

	if (snapshot_commands(debug_root, &before)) {
		fail("AdminQ debugfs", "mount debugfs and run as root");
		goto cleanup;
	}
	debug_ok = true;
	pass("AdminQ debugfs", debug_root);

	ctx = ibv_open_device(device);
	if (!ctx) {
		fail("ALLOC_UCTX/open", strerror(errno));
		goto cleanup;
	}
	opened = true;
	pass("ALLOC_UCTX/open", "ibv_open_device succeeded");
	if (opts.vector >= (unsigned int)ctx->num_comp_vectors) {
		snprintf(detail, sizeof(detail), "vector %u >= available %d",
			 opts.vector, ctx->num_comp_vectors);
		fail("completion vector", detail);
		goto cleanup;
	}

	if (ibv_query_device(ctx, &device_attr)) {
		fail("query device", strerror(errno));
		goto cleanup;
	}
	if (opts.port > device_attr.phys_port_cnt ||
	    ibv_query_port(ctx, opts.port, &port_attr)) {
		fail("query port", strerror(errno));
		goto cleanup;
	}
	snprintf(detail, sizeof(detail), "max_pd=%d max_mr=%d max_cq=%d max_qp=%d",
		 device_attr.max_pd, device_attr.max_mr, device_attr.max_cq,
		 device_attr.max_qp);
	pass("device capabilities", detail);
	snprintf(detail, sizeof(detail), "state=%d active_mtu=%d gid_tbl_len=%d",
		 (int)port_attr.state, (int)port_attr.active_mtu,
		 port_attr.gid_tbl_len);
	pass("port attributes", detail);
	if (!netdev[0])
		discover_gid_netdev(dev_name, opts.port, port_attr.gid_tbl_len,
				    netdev, sizeof(netdev));
	if (netdev[0])
		printf("  bound netdev=%s\n", netdev);

	gid_count = enumerate_gids(ctx, opts.port, port_attr.gid_tbl_len,
				   &first_gid, &first_gid_index);
	if (gid_count > 0)
		pass("GID query", "non-zero RoCE GID found");
	else
		fail("GID query", "no non-zero GID; configure an address on bound netdev");

	if (opts.gid_cidr) {
		if (!netdev[0])
			fail("GID ADD/DEL", "bound netdev not found");
		else
			test_gid_cycle(debug_root, netdev, opts.gid_cidr);
		gid_count = enumerate_gids(ctx, opts.port, port_attr.gid_tbl_len,
					   &first_gid, &first_gid_index);
	} else {
		warn("GID ADD/DEL", "not cycled; use --gid-cycle <unused-CIDR>");
	}

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		fail("PD", strerror(errno));
		goto cleanup;
	}
	pd_created = true;
	pass("PD", "allocated");

	if (posix_memalign(&buffer, (size_t)sysconf(_SC_PAGESIZE), opts.mr_size)) {
		fail("MR buffer", "posix_memalign failed");
		goto cleanup;
	}
	memset(buffer, 0xa5, opts.mr_size);
	mr = ibv_reg_mr(pd, buffer, opts.mr_size,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
			IBV_ACCESS_REMOTE_WRITE);
	if (!mr) {
		fail("MR", strerror(errno));
		goto cleanup;
	}
	mr_created = true;
	snprintf(detail, sizeof(detail), "length=%zu lkey=%#x rkey=%#x",
		 opts.mr_size, mr->lkey, mr->rkey);
	pass("MR", detail);

	cq = ibv_create_cq(ctx, 64, NULL, NULL, opts.vector);
	if (!cq) {
		fail("EQ/CQ", strerror(errno));
		goto cleanup;
	}
	cq_created = true;
	pass("EQ/CQ", "CQ created; completion EQ is created lazily by driver");

	qp_init.send_cq = cq;
	qp_init.recv_cq = cq;
	qp_init.qp_type = IBV_QPT_RC;
	qp_init.sq_sig_all = 0;
	qp_init.cap.max_send_wr = 8;
	qp_init.cap.max_recv_wr = 8;
	qp_init.cap.max_send_sge = 1;
	qp_init.cap.max_recv_sge = 1;
	qp = ibv_create_qp(pd, &qp_init);
	if (!qp) {
		fail("QP", strerror(errno));
		goto cleanup;
	}
	qp_created = true;
	snprintf(detail, sizeof(detail), "RC QPN=%u", qp->qp_num);
	pass("QP", detail);
	query_qp_state(qp, IBV_QPS_RESET);

	if (!qp_to_init(qp, opts.port))
		qp_init_done = true;
	if (qp_init_done && !opts.basic_qp) {
		if (!gid_count) {
			fail("QP full state machine", "requires a non-zero GID");
		} else if (!qp_to_rts(qp, opts.port,
				      port_attr.active_mtu ? port_attr.active_mtu : IBV_MTU_1024,
				      &first_gid, first_gid_index)) {
			qp_full_done = true;
		}
	} else if (opts.basic_qp) {
		warn("QP full state machine", "skipped by --basic-qp");
	}

	test_debug_objects(debug_root);
	if (read_hw_stats(dev_name, opts.port)) {
		fail("RDMA hw statistics", "one or more representative counters missing");
	} else {
		stats_done = true;
		pass("RDMA hw statistics", "GET_STATS/EXT/EXT2/EXT3 path read");
	}

	printf("Waiting %.1f seconds for idle AdminQ HEALTH_CHECK...\n",
	       HEALTH_IDLE_US / 1000000.0);
	usleep(HEALTH_IDLE_US);
	if (!stop_requested && !test_devlink_health(bdf))
		health_done = true;

cleanup:
	if (qp) {
		if (ibv_destroy_qp(qp))
			fail("QP destroy", strerror(errno));
		else
			pass("QP destroy", "destroyed");
	}
	if (cq) {
		if (ibv_destroy_cq(cq))
			fail("CQ destroy", strerror(errno));
		else
			pass("CQ destroy", "destroyed");
	}
	if (mr) {
		if (ibv_dereg_mr(mr))
			fail("MR deregister", strerror(errno));
		else
			pass("MR deregister", "deregistered");
	}
	if (pd) {
		if (ibv_dealloc_pd(pd))
			fail("PD deallocate", strerror(errno));
		else
			pass("PD deallocate", "deallocated");
	}
	free(buffer);
	if (ctx) {
		if (ibv_close_device(ctx))
			fail("DEALLOC_UCTX/close", strerror(errno));
		else
			pass("DEALLOC_UCTX/close", "ibv_close_device succeeded");
	}
	if (device_list)
		ibv_free_device_list(device_list);

	if (debug_ok && !snapshot_commands(debug_root, &after)) {
		expect_command(&before, &after, "GET_CAP", true);
		expect_command(&before, &after, "GET_CAP_EXT", true);
		if (gid_count > 0 && !opts.gid_cidr)
			expect_command(&before, &after, "ADD_GID", true);
		if (opened) {
			expect_command(&before, &after, "ALLOC_UCTX", false);
			expect_command(&before, &after, "DEALLOC_UCTX", false);
		}
		if (pd_created) {
			expect_command(&before, &after, "ALLOC_PD", false);
			expect_command(&before, &after, "DEALLOC_PD", false);
		}
		if (mr_created) {
			expect_command(&before, &after, "REG_MR", false);
			expect_command(&before, &after, "DEREG_MR", false);
		}
		if (cq_created) {
			expect_command(&before, &after, "CREATE_EQ", true);
			expect_command(&before, &after, "CREATE_CQ", false);
			expect_command(&before, &after, "DESTROY_CQ", false);
		}
		if (qp_created) {
			expect_command(&before, &after, "CREATE_QP", false);
			expect_command(&before, &after, "QUERY_QP", false);
			expect_command(&before, &after, "DESTROY_QP", false);
		}
		if (qp_init_done)
			expect_command(&before, &after, "RST2INIT_QP", false);
		if (qp_full_done) {
			expect_command(&before, &after, "INIT2RTR_QP", false);
			expect_command(&before, &after, "RTR2RTS_QP", false);
			expect_command(&before, &after, "2ERR_QP", false);
			expect_command(&before, &after, "2RST_QP", false);
		}
		if (stats_done) {
			expect_command(&before, &after, "GET_STATS", false);
			expect_command(&before, &after, "GET_STATS_EXT", false);
			expect_command(&before, &after, "GET_STATS_EXT2", false);
			expect_command(&before, &after, "GET_STATS_EXT3", false);
		}
		if (health_done)
			expect_command(&before, &after, "HEALTH_CHECK", false);
	}

	printf("\nSummary: PASS=%u WARN=%u FAIL=%u\n", passes, warnings, failures);
	if (stop_requested)
		return 130;
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
