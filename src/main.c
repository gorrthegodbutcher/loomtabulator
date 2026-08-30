#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <pthread.h>
#include <rte_eal.h>
#include <rte_ring.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_launch.h>
#include <rte_malloc.h>

#include "record.h"
#include "ring_input.h"
#include "graph_config.h"
#include "pipeline.h"
#include "pipeline_worker.h"
#include "epoch_barrier.h"
#include "plugin_loader.h"
#include "testgen.h"
#include "web_status.h"

#define STATUS_UPDATE_INTERVAL_US 500000

static volatile bool g_shutdown_requested;

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
		g_shutdown_requested = true;
}

/* Reads path's raw bytes into a malloc'd, NUL-terminated buffer - just
 * for seeding web_graph_ctx.current_graph_json with the startup graph's
 * own text (see web_status.h), so GET /api/graph has something to serve
 * before any POST ever happens. graph_config_load() has already proven
 * path opens and parses cleanly by the time this is called, so failure
 * here is unexpected enough to just warn and leave current_graph_json
 * NULL (GET /api/graph then reports 501) rather than aborting the whole
 * run over a display-only feature. */
static char *
read_file_contents(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 0) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	size_t n = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[n] = '\0';
	if (out_len != NULL)
		*out_len = n;
	return buf;
}

struct app_opts {
	const char *graph_path;
	uint16_t web_port;
	const char *web_root;
	const char *plugins_dir;
	bool testgen_enabled; /* off by default - see --testgen-enable below */
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
		"  --web-root=PATH     directory holding the built web/dist/ (Phase 3\n"
		"                      UI) to serve as static files, default\n"
		"                      ../web/dist relative to cwd (empty string = off)\n"
		"  --plugins-dir=PATH  directory to scan for stage plugin *.so files\n"
		"                      (built-in stages are plugins too - see\n"
		"                      plugin-sdk/README.md), default ../plugins\n"
		"                      relative to cwd (empty string = load none)\n"
		"  --workers=N         worker lcores to run the pipeline on, default:\n"
		"                      (EAL lcore count - 1). Must be <= that.\n"
		"\n"
		"  Synthetic input (stands in for chrontabulator's not-yet-built\n"
		"  replay feature - see the project plan's Phase 4) - off by default,\n"
		"  so a real external producer (e.g. a DPDK secondary process\n"
		"  attaching to this same ring) has sole control of the input ring\n"
		"  unless you explicitly ask for synthetic traffic too:\n"
		"  --testgen-enable    start the synthetic generator (default: off)\n"
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
		{"web-root", required_argument, 0, 'R'},
		{"plugins-dir", required_argument, 0, 'P'},
		{"workers", required_argument, 0, 'N'},
		{"testgen-enable", no_argument, 0, 'e'},
		{"testgen-rate", required_argument, 0, 'r'},
		{"testgen-count", required_argument, 0, 'c'},
		{"testgen-payload", required_argument, 0, 'p'},
		{"testgen-barrier-every", required_argument, 0, 'b'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	memset(opts, 0, sizeof(*opts));
	opts->web_port = 8080;
	opts->web_root = "../web/dist";
	opts->plugins_dir = "../plugins";
	opts->testgen_payload_len = 16;

	optind = 1;
	int opt;
	while ((opt = getopt_long(argc, argv, "g:W:R:P:N:er:c:p:b:h", long_options, NULL)) != -1) {
		switch (opt) {
		case 'g': opts->graph_path = optarg; break;
		case 'W': opts->web_port = (uint16_t)strtoul(optarg, NULL, 10); break;
		case 'R': opts->web_root = optarg; break;
		case 'P': opts->plugins_dir = optarg; break;
		case 'N': opts->workers = (unsigned int)strtoul(optarg, NULL, 10); break;
		case 'e': opts->testgen_enabled = true; break;
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

	char errbuf[256];
	if (!plugin_loader_load(opts.plugins_dir, errbuf, sizeof(errbuf)))
		rte_exit(EXIT_FAILURE, "failed to load plugins from '%s': %s\n",
			  opts.plugins_dir, errbuf);
	printf("Loaded %zu stage plugin(s) from '%s'\n", stage_registry_count(), opts.plugins_dir);

	struct pipeline_chain chain;
	struct graph_config_result graph_info;
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

	struct web_graph_ctx graph_ctx = { .graph_path = opts.graph_path };
	graph_ctx.current_graph_json = read_file_contents(opts.graph_path, &graph_ctx.current_graph_len);
	if (graph_ctx.current_graph_json == NULL)
		fprintf(stderr, "loomtabulator: warning: couldn't re-read '%s' for GET /api/graph "
				 "(status.json and the running pipeline are unaffected)\n", opts.graph_path);

	if (opts.web_port != 0 &&
	    web_status_start(opts.web_port, &status, &g_shutdown_requested, opts.web_root, &graph_ctx) == 0)
		printf("Status server listening on port %u\n", opts.web_port);

	/* Off by default (see --testgen-enable) - the input ring is a real
	 * external interface now (a DPDK secondary process can attach and
	 * enqueue directly, same mechanism Phase 4's chrontabulator
	 * integration will eventually use - see ring_input.h), and an
	 * always-on synthetic generator competing with real traffic on
	 * that same ring is exactly the kind of surprise a test session
	 * feeding real data shouldn't have to work around. */
	struct testgen_config tg_cfg = {
		.ring = ring,
		.rate_per_sec = opts.testgen_rate,
		.count = opts.testgen_count,
		.payload_len = opts.testgen_payload_len,
		.barrier_every = opts.testgen_barrier_every,
	};
	pthread_t testgen_thread;
	bool testgen_started = false;
	if (opts.testgen_enabled) {
		if (pthread_create(&testgen_thread, NULL, testgen_run, &tg_cfg) != 0)
			rte_exit(EXIT_FAILURE, "failed to start testgen thread\n");
		testgen_started = true;
	}

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
	if (testgen_started) {
		testgen_stop();
		pthread_join(testgen_thread, NULL);
	}

	rte_eal_mp_wait_lcore();
	free(worker_ctxs);

	/* Drain anything left in the ring (data or barrier records - both
	 * freed the same way at shutdown, no special case needed since the
	 * epoch_barrier's final state no longer matters), so the rte_malloc'd
	 * blobs don't leak. rte_free(), not free() - see pipeline_worker.c's
	 * own comment on why every ring item, from any producer, is
	 * rte_malloc()'d. */
	void *leftover = NULL;
	while (rte_ring_dequeue(ring, &leftover) == 0)
		rte_free(leftover);

	web_status_stop();
	app_web_status_destroy(&status);
	free(graph_ctx.current_graph_json);

	for (size_t i = 0; i < chain.stage_count; i++)
		if (chain.stages[i].stage->teardown != NULL)
			chain.stages[i].stage->teardown(chain.stages[i].state);

	/* Only safe here, after every stage's own teardown() above AND
	 * after rte_eal_mp_wait_lcore()/pthread_join(testgen_thread) above
	 * have already returned - no thread can still be mid-call into a
	 * plugin's code at this point, so dlclose()-ing every handle can't
	 * unmap code out from under a live caller. See plugin_loader.h. */
	plugin_loader_shutdown();

	printf("Final counts: in=%" PRIu64 " dropped=%" PRIu64 " forwarded=%" PRIu64 "\n",
	       (uint64_t)atomic_load(&counters.records_in),
	       (uint64_t)atomic_load(&counters.records_dropped),
	       (uint64_t)atomic_load(&counters.records_forwarded));

	rte_eal_cleanup();
	return 0;
}
