#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <pthread.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_launch.h>

#include "record.h"
#include "port_init.h"
#include "ring_input.h"
#include "graph_config.h"
#include "pipeline.h"
#include "pipeline_worker.h"
#include "epoch_barrier.h"
#include "testgen.h"
#include "web_status.h"
#include "stages/forward_udp_stage.h"

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 512
#define MBUF_DATA_SIZE 9216
#define STATUS_UPDATE_INTERVAL_US 500000

static volatile bool g_shutdown_requested;

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
		g_shutdown_requested = true;
}

struct app_opts {
	const char *graph_path;
	uint16_t web_port;
	uint16_t mtu;
	bool force_10g;
	uint32_t testgen_rate;
	uint64_t testgen_count;
	uint32_t testgen_payload_len;
	uint64_t testgen_barrier_every;
	unsigned int workers; /* 0 = derive from rte_lcore_count() - 1 */
};

static void
usage(void)
{
	printf(
		"Loomtabulator usage:\n"
		"  <EAL args> -- --graph=PATH [options]\n"
		"\n"
		"  --graph=PATH        JSON pipeline graph to load (required) - see\n"
		"                      testdata/example_graph.json for the v1 shape.\n"
		"  --web-port=N        status.json port, default 8080 (0 = off)\n"
		"  --mtu=BYTES         port MTU (default: device default, 1500)\n"
		"  --force-10g         restrict advertised link speed to 10G only\n"
		"  --workers=N         worker lcores to run the pipeline on, default:\n"
		"                      (EAL lcore count - 1). Must be <= that.\n"
		"\n"
		"  Synthetic input (stands in for chrontabulator's not-yet-built\n"
		"  replay feature - see the project plan's Phase 4):\n"
		"  --testgen-rate=N    records/sec, default 0 (as fast as possible)\n"
		"  --testgen-count=N   records to send, default 0 (unlimited)\n"
		"  --testgen-payload=N payload bytes per record, >= 8, default 16\n"
		"  --testgen-barrier-every=N\n"
		"                      insert an epoch barrier record every N data\n"
		"                      records, default 0 (never - Phase 2 manual\n"
		"                      smoke-test aid, see the project plan)\n");
}

static int
parse_args(int argc, char **argv, struct app_opts *opts)
{
	static const struct option long_options[] = {
		{"graph", required_argument, 0, 'g'},
		{"web-port", required_argument, 0, 'W'},
		{"mtu", required_argument, 0, 'u'},
		{"force-10g", no_argument, 0, 'F'},
		{"workers", required_argument, 0, 'N'},
		{"testgen-rate", required_argument, 0, 'r'},
		{"testgen-count", required_argument, 0, 'c'},
		{"testgen-payload", required_argument, 0, 'p'},
		{"testgen-barrier-every", required_argument, 0, 'b'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	memset(opts, 0, sizeof(*opts));
	opts->web_port = 8080;
	opts->testgen_payload_len = 16;

	optind = 1;
	int opt;
	while ((opt = getopt_long(argc, argv, "g:W:u:FN:r:c:p:b:h", long_options, NULL)) != -1) {
		switch (opt) {
		case 'g': opts->graph_path = optarg; break;
		case 'W': opts->web_port = (uint16_t)strtoul(optarg, NULL, 10); break;
		case 'u': opts->mtu = (uint16_t)strtoul(optarg, NULL, 10); break;
		case 'F': opts->force_10g = true; break;
		case 'N': opts->workers = (unsigned int)strtoul(optarg, NULL, 10); break;
		case 'r': opts->testgen_rate = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'c': opts->testgen_count = strtoull(optarg, NULL, 10); break;
		case 'p': opts->testgen_payload_len = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'b': opts->testgen_barrier_every = strtoull(optarg, NULL, 10); break;
		case 'h': usage(); exit(0);
		default: usage(); return -1;
		}
	}

	if (opts->graph_path == NULL) {
		fprintf(stderr, "--graph=PATH is required\n");
		usage();
		return -1;
	}
	if (opts->testgen_payload_len < 8) {
		fprintf(stderr, "--testgen-payload must be at least 8\n");
		return -1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int eal_argc = rte_eal_init(argc, argv);
	if (eal_argc < 0)
		rte_exit(EXIT_FAILURE, "rte_eal_init failed: %s\n", rte_strerror(rte_errno));
	argc -= eal_argc;
	argv += eal_argc;

	struct app_opts opts;
	if (parse_args(argc, argv, &opts) != 0)
		return 1;

	unsigned int n_workers = opts.workers != 0 ? opts.workers : rte_lcore_count() - 1;
	if (n_workers == 0 || n_workers > rte_lcore_count() - 1)
		rte_exit(EXIT_FAILURE,
			  "need at least 1 worker lcore beyond the main lcore "
			  "(have %u lcore(s) total, requested %u worker(s))\n",
			  rte_lcore_count(), n_workers);

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	if (rte_eth_dev_count_avail() == 0)
		rte_exit(EXIT_FAILURE, "no DPDK-bound NIC ports found\n");
	uint16_t port = 0;

	struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create(
		"LOOM_MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, MBUF_DATA_SIZE, rte_socket_id());
	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "cannot create mbuf pool: %s\n", rte_strerror(rte_errno));

	struct port_init_result pir;
	if (app_port_init(port, mbuf_pool, opts.mtu, false, opts.force_10g, &pir) != 0)
		rte_exit(EXIT_FAILURE, "failed to initialize port %u\n", port);
	printf("Port %u ready: link %s, %u Mbps, MTU %u, HW checksum offload %s\n",
	       port, pir.link_up ? "UP" : "DOWN", pir.link_speed_mbps, pir.actual_mtu,
	       pir.tx_checksum_capable ? "available" : "not supported by this NIC");

	forward_udp_stage_set_runtime(mbuf_pool, port, pir.mac_addr.addr_bytes);

	struct pipeline_chain chain;
	struct graph_config_result graph_info;
	char errbuf[256];
	if (!graph_config_load(opts.graph_path, &chain, &graph_info, errbuf, sizeof(errbuf)))
		rte_exit(EXIT_FAILURE, "failed to load graph '%s': %s\n", opts.graph_path, errbuf);
	printf("Loaded graph '%s': %zu stage(s), input ring '%s' (size %u)\n",
	       opts.graph_path, chain.stage_count, graph_info.ring_name, graph_info.ring_size);
	printf("Running the pipeline on %u worker lcore(s)\n", n_workers);

	struct rte_ring *ring = ring_input_create(graph_info.ring_name, graph_info.ring_size);
	if (ring == NULL)
		rte_exit(EXIT_FAILURE, "failed to create ring '%s': %s\n",
			  graph_info.ring_name, rte_strerror(rte_errno));

	struct pipeline_counters counters;
	pipeline_counters_init(&counters);

	struct epoch_barrier barrier;
	epoch_barrier_init(&barrier);

	struct app_web_status status;
	app_web_status_init(&status);
	if (opts.web_port != 0 && web_status_start(opts.web_port, &status, &g_shutdown_requested) == 0)
		printf("Status server listening on port %u\n", opts.web_port);

	struct testgen_config tg_cfg = {
		.ring = ring,
		.rate_per_sec = opts.testgen_rate,
		.count = opts.testgen_count,
		.payload_len = opts.testgen_payload_len,
		.barrier_every = opts.testgen_barrier_every,
	};
	pthread_t testgen_thread;
	if (pthread_create(&testgen_thread, NULL, testgen_run, &tg_cfg) != 0)
		rte_exit(EXIT_FAILURE, "failed to start testgen thread\n");

	struct pipeline_worker_ctx *worker_ctxs = calloc(n_workers, sizeof(*worker_ctxs));
	if (worker_ctxs == NULL)
		rte_exit(EXIT_FAILURE, "out of memory allocating %u worker context(s)\n", n_workers);
	for (unsigned int i = 0; i < n_workers; i++) {
		worker_ctxs[i].ring = ring;
		worker_ctxs[i].chain = &chain;
		worker_ctxs[i].counters = &counters;
		worker_ctxs[i].barrier = &barrier;
		worker_ctxs[i].stop_requested = &g_shutdown_requested;
		worker_ctxs[i].worker_id = i;
	}

	unsigned int lcore_id, launched = 0;
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (launched >= n_workers)
			break;
		rte_eal_remote_launch(pipeline_worker_lcore_main, &worker_ctxs[launched], lcore_id);
		launched++;
	}

	printf("Running - Ctrl+C to stop.\n");
	uint64_t last_status_update = rte_rdtsc();
	uint64_t status_interval_tsc = rte_get_tsc_hz() * STATUS_UPDATE_INTERVAL_US / 1000000;

	while (!g_shutdown_requested) {
		rte_delay_us(200);

		uint64_t now = rte_rdtsc();
		if (now - last_status_update >= status_interval_tsc) {
			app_web_status_update(&status,
					       atomic_load(&counters.records_in),
					       atomic_load(&counters.records_dropped),
					       atomic_load(&counters.records_forwarded));
			last_status_update = now;
		}
	}

	printf("\nShutting down...\n");
	testgen_stop();
	pthread_join(testgen_thread, NULL);

	rte_eal_mp_wait_lcore();
	free(worker_ctxs);

	/* Drain anything left in the ring (data or barrier records - both
	 * freed the same way at shutdown, no special case needed since the
	 * epoch_barrier's final state no longer matters), so the malloc'd
	 * blobs don't leak. */
	void *leftover = NULL;
	while (rte_ring_dequeue(ring, &leftover) == 0)
		free(leftover);

	web_status_stop();
	app_web_status_destroy(&status);

	for (size_t i = 0; i < chain.stage_count; i++)
		if (chain.stages[i].stage->teardown != NULL)
			chain.stages[i].stage->teardown(chain.stages[i].state);

	printf("Final counts: in=%" PRIu64 " dropped=%" PRIu64 " forwarded=%" PRIu64 "\n",
	       (uint64_t)atomic_load(&counters.records_in),
	       (uint64_t)atomic_load(&counters.records_dropped),
	       (uint64_t)atomic_load(&counters.records_forwarded));

	rte_eth_dev_stop(port);
	rte_eth_dev_close(port);
	rte_delay_us(3000000);
	rte_eal_cleanup();
	return 0;
}
