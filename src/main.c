#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_cycles.h>
#include <rte_errno.h>

#include "record.h"
#include "port_init.h"
#include "ring_input.h"
#include "graph_config.h"
#include "pipeline.h"
#include "testgen.h"
#include "web_status.h"
#include "stages/forward_udp_stage.h"

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 512
#define MBUF_DATA_SIZE 9216
#define RING_BURST 32
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
		"\n"
		"  Synthetic input (stands in for chrontabulator's not-yet-built\n"
		"  replay feature - see the project plan's Phase 4):\n"
		"  --testgen-rate=N    records/sec, default 0 (as fast as possible)\n"
		"  --testgen-count=N   records to send, default 0 (unlimited)\n"
		"  --testgen-payload=N payload bytes per record, >= 8, default 16\n");
}

static int
parse_args(int argc, char **argv, struct app_opts *opts)
{
	static const struct option long_options[] = {
		{"graph", required_argument, 0, 'g'},
		{"web-port", required_argument, 0, 'W'},
		{"mtu", required_argument, 0, 'u'},
		{"force-10g", no_argument, 0, 'F'},
		{"testgen-rate", required_argument, 0, 'r'},
		{"testgen-count", required_argument, 0, 'c'},
		{"testgen-payload", required_argument, 0, 'p'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	memset(opts, 0, sizeof(*opts));
	opts->web_port = 8080;
	opts->testgen_payload_len = 16;

	optind = 1;
	int opt;
	while ((opt = getopt_long(argc, argv, "g:W:u:Fr:c:p:h", long_options, NULL)) != -1) {
		switch (opt) {
		case 'g': opts->graph_path = optarg; break;
		case 'W': opts->web_port = (uint16_t)strtoul(optarg, NULL, 10); break;
		case 'u': opts->mtu = (uint16_t)strtoul(optarg, NULL, 10); break;
		case 'F': opts->force_10g = true; break;
		case 'r': opts->testgen_rate = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'c': opts->testgen_count = strtoull(optarg, NULL, 10); break;
		case 'p': opts->testgen_payload_len = (uint32_t)strtoul(optarg, NULL, 10); break;
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

	struct pipeline pl;
	struct graph_config_result graph_info;
	char errbuf[256];
	if (!graph_config_load(opts.graph_path, &pl, &graph_info, errbuf, sizeof(errbuf)))
		rte_exit(EXIT_FAILURE, "failed to load graph '%s': %s\n", opts.graph_path, errbuf);
	printf("Loaded graph '%s': %zu stage(s), input ring '%s' (size %u)\n",
	       opts.graph_path, pl.stage_count, graph_info.ring_name, graph_info.ring_size);

	struct rte_ring *ring = ring_input_create(graph_info.ring_name, graph_info.ring_size);
	if (ring == NULL)
		rte_exit(EXIT_FAILURE, "failed to create ring '%s': %s\n",
			  graph_info.ring_name, rte_strerror(rte_errno));

	struct app_web_status status;
	app_web_status_init(&status);
	if (opts.web_port != 0 && web_status_start(opts.web_port, &status, &g_shutdown_requested) == 0)
		printf("Status server listening on port %u\n", opts.web_port);

	struct testgen_config tg_cfg = {
		.ring = ring,
		.rate_per_sec = opts.testgen_rate,
		.count = opts.testgen_count,
		.payload_len = opts.testgen_payload_len,
	};
	pthread_t testgen_thread;
	if (pthread_create(&testgen_thread, NULL, testgen_run, &tg_cfg) != 0)
		rte_exit(EXIT_FAILURE, "failed to start testgen thread\n");

	printf("Running - Ctrl+C to stop.\n");
	uint64_t last_status_update = rte_rdtsc();
	uint64_t status_interval_tsc = rte_get_tsc_hz() * STATUS_UPDATE_INTERVAL_US / 1000000;

	void *bufs[RING_BURST];
	while (!g_shutdown_requested) {
		unsigned int n = rte_ring_dequeue_burst(ring, bufs, RING_BURST, NULL);
		if (n == 0) {
			rte_delay_us(200);
		} else {
			for (unsigned int i = 0; i < n; i++) {
				uint8_t *blob = bufs[i];
				struct chrono_record_hdr *hdr = (struct chrono_record_hdr *)blob;
				pipeline_run(&pl, blob, (uint32_t)(sizeof(*hdr) + hdr->len), hdr->capture_tsc);
				free(blob);
			}
		}

		uint64_t now = rte_rdtsc();
		if (now - last_status_update >= status_interval_tsc) {
			app_web_status_update(&status, pl.records_in, pl.records_dropped, pl.records_forwarded);
			last_status_update = now;
		}
	}

	printf("\nShutting down...\n");
	testgen_stop();
	pthread_join(testgen_thread, NULL);

	/* Drain anything testgen enqueued right before it stopped, so the
	 * malloc'd blobs don't leak. */
	void *leftover = NULL;
	while (rte_ring_dequeue(ring, &leftover) == 0)
		free(leftover);

	web_status_stop();
	app_web_status_destroy(&status);

	for (size_t i = 0; i < pl.stage_count; i++)
		if (pl.stages[i].stage->teardown != NULL)
			pl.stages[i].stage->teardown(pl.stages[i].state);

	printf("Final counts: in=%" PRIu64 " dropped=%" PRIu64 " forwarded=%" PRIu64 "\n",
	       pl.records_in, pl.records_dropped, pl.records_forwarded);

	rte_eth_dev_stop(port);
	rte_eth_dev_close(port);
	rte_delay_us(3000000);
	rte_eal_cleanup();
	return 0;
}
