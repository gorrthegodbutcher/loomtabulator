#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <rte_eal.h>
#include <rte_ring.h>
#include <rte_malloc.h>
#include "../src/record.h"
#include "../src/pipeline.h"
#include "../src/pipeline_worker.h"
#include "../src/ring_input.h"

/* Integration test for Phase 2: exercises epoch_barrier.c and
 * pipeline_worker.c together against a REAL rte_ring (unlike
 * test_epoch_barrier.c, which validates epoch_barrier.c's state machine
 * alone against a fake in-memory FIFO) - proves the actual guarantee the
 * whole design leans on (rte_ring's multi-consumer dequeue preserves
 * overall FIFO order across all consumers combined) holds end to end. No
 * hardware needed (--no-huge --no-pci, no port/mbuf-pool setup at all -
 * this test never touches a NIC).
 *
 * pipeline_worker_lcore_main() is called directly from plain pthreads
 * rather than via rte_eal_remote_launch() - its signature is a plain
 * int(*)(void*) that only touches rte_ring/atomics, no lcore-specific
 * API, so this sidesteps needing multiple `-l` cores reserved for the
 * test. Uses a trivial local "identity" stage instead of a real graph
 * (graph_config.c/stage_registry.c), so this test needs no DPDK
 * mbuf/port involvement at all - only the ring and the worker/barrier
 * logic are under test here. */

#define NUM_EPOCHS 6
#define ITEMS_PER_EPOCH 200
#define NUM_WORKERS 4
#define RING_SIZE 4096

static struct stage_result
identity_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	(void)state;
	memcpy(out->data, in->data, in->len);
	out->type = PORT_TYPE_RAW_RECORD;
	out->len = in->len;
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true };
}

static const struct stage g_identity_stage = {
	.name = "identity",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD),
	.out_type = PORT_TYPE_RAW_RECORD,
	.init = NULL,
	.process = identity_process,
	.teardown = NULL,
};

struct completion {
	uint64_t epoch;
	uint64_t seq;
	uint64_t order;
};

static struct completion g_completions[NUM_EPOCHS * ITEMS_PER_EPOCH];
static _Atomic size_t g_completions_next;
static _Atomic uint64_t g_completion_order;

static void
on_record_processed(void *cb_arg, uint64_t epoch, uint64_t seq)
{
	(void)cb_arg;
	uint64_t order = atomic_fetch_add(&g_completion_order, 1);
	size_t idx = atomic_fetch_add(&g_completions_next, 1);
	g_completions[idx] = (struct completion){ .epoch = epoch, .seq = seq, .order = order };
}

static volatile bool g_stop_requested;

/* pthread_create() needs a void *(*)(void *) entry point;
 * pipeline_worker_lcore_main() is int(*)(void*) (its actual contract is
 * rte_eal_remote_launch()-compatible - see pipeline_worker.h) - a thin
 * trampoline instead of casting the function pointer directly avoids
 * relying on int/void* return-slot compatibility being ABI-safe. */
static void *
worker_thread_trampoline(void *arg)
{
	pipeline_worker_lcore_main(arg);
	return NULL;
}

static void *
producer_main(void *arg)
{
	struct rte_ring *ring = arg;

	for (uint64_t e = 0; e < NUM_EPOCHS; e++) {
		for (uint64_t s = 0; s < ITEMS_PER_EPOCH; s++) {
			/* rte_malloc(), not malloc() - pipeline_worker.c's
			 * consumer side now calls rte_free() unconditionally
			 * on every dequeued item (see ring_input.h's own
			 * comment on why every producer must agree on this). */
			struct chrono_record_hdr *hdr = rte_malloc(NULL, sizeof(*hdr), 0);
			assert(hdr != NULL);
			hdr->magic = CHRONO_RECORD_MAGIC;
			hdr->seq = s;
			hdr->capture_tsc = 0;
			hdr->len = 0;
			hdr->reserved = 0;
			while (rte_ring_enqueue(ring, hdr) != 0)
				usleep(100); /* ring momentarily full - retry, don't drop */
		}

		struct chrono_record_hdr *barrier = rte_malloc(NULL, sizeof(*barrier), 0);
		assert(barrier != NULL);
		barrier->magic = CHRONO_BARRIER_MAGIC;
		barrier->seq = e;
		barrier->capture_tsc = 0;
		barrier->len = 0;
		barrier->reserved = 0;
		while (rte_ring_enqueue(ring, barrier) != 0)
			usleep(100);
	}

	return NULL;
}

int
main(void)
{
	char *eal_args[] = { "test_pipeline_workers", "--no-huge", "-m", "128", "--no-pci", "-l", "0" };
	int eal_argc = rte_eal_init(sizeof(eal_args) / sizeof(eal_args[0]), eal_args);
	assert(eal_argc >= 0);

	bool owns_ring = false;
	struct rte_ring *ring = ring_input_create("TEST_PIPELINE_WORKERS_RING", RING_SIZE, &owns_ring);
	assert(ring != NULL);
	assert(owns_ring); /* nothing else could have created this ring first */

	struct pipeline_chain chain = { .stage_count = 1 };
	chain.stages[0].stage = &g_identity_stage;
	chain.stages[0].state = NULL;

	struct pipeline_counters counters;
	pipeline_counters_init(&counters);

	struct epoch_barrier barrier;
	epoch_barrier_init(&barrier);

	struct pipeline_worker_ctx worker_ctxs[NUM_WORKERS];
	memset(worker_ctxs, 0, sizeof(worker_ctxs));
	pthread_t worker_threads[NUM_WORKERS];
	for (int i = 0; i < NUM_WORKERS; i++) {
		worker_ctxs[i].ring = ring;
		worker_ctxs[i].chain = &chain;
		worker_ctxs[i].counters = &counters;
		worker_ctxs[i].barrier = &barrier;
		worker_ctxs[i].stop_requested = &g_stop_requested;
		worker_ctxs[i].worker_id = (unsigned int)i;
		worker_ctxs[i].on_record_processed = on_record_processed;
		assert(pthread_create(&worker_threads[i], NULL, worker_thread_trampoline,
				       &worker_ctxs[i]) == 0);
	}

	pthread_t producer_thread;
	assert(pthread_create(&producer_thread, NULL, producer_main, ring) == 0);
	pthread_join(producer_thread, NULL);

	while (atomic_load(&g_completions_next) < NUM_EPOCHS * ITEMS_PER_EPOCH ||
	       epoch_barrier_current_epoch(&barrier) < NUM_EPOCHS)
		usleep(1000);

	g_stop_requested = true;
	for (int i = 0; i < NUM_WORKERS; i++)
		pthread_join(worker_threads[i], NULL);

	size_t n = atomic_load(&g_completions_next);
	assert(n == NUM_EPOCHS * ITEMS_PER_EPOCH);
	printf("PASS: all %zu data records across %d epochs completed through a real "
	       "multi-consumer rte_ring with %d workers\n", n, NUM_EPOCHS, NUM_WORKERS);

	assert(epoch_barrier_current_epoch(&barrier) == NUM_EPOCHS);
	printf("PASS: all %d barriers drained, epoch counter advanced correctly\n", NUM_EPOCHS);

	/* The core assertion: for every pair of adjacent epochs, every
	 * completion-order index for epoch K precedes every completion-order
	 * index for epoch K+1 - a hard cliff at each boundary, checked
	 * end-to-end through the real ring. */
	for (uint64_t e = 0; e + 1 < NUM_EPOCHS; e++) {
		uint64_t max_order_this = 0, min_order_next = UINT64_MAX;
		for (size_t i = 0; i < n; i++) {
			if (g_completions[i].epoch == e && g_completions[i].order > max_order_this)
				max_order_this = g_completions[i].order;
			if (g_completions[i].epoch == e + 1 && g_completions[i].order < min_order_next)
				min_order_next = g_completions[i].order;
		}
		assert(max_order_this < min_order_next);
	}
	printf("PASS: zero interleaving across every epoch boundary (real ring, %d epochs checked)\n",
	       NUM_EPOCHS - 1);

	/* Every epoch has exactly ITEMS_PER_EPOCH completions, and seq
	 * values 0..ITEMS_PER_EPOCH-1 each appear exactly once per epoch -
	 * catches drops/duplication as a bonus, not just ordering. */
	for (uint64_t e = 0; e < NUM_EPOCHS; e++) {
		bool seen[ITEMS_PER_EPOCH] = {0};
		size_t count = 0;
		for (size_t i = 0; i < n; i++) {
			if (g_completions[i].epoch != e)
				continue;
			assert(g_completions[i].seq < ITEMS_PER_EPOCH);
			assert(!seen[g_completions[i].seq]);
			seen[g_completions[i].seq] = true;
			count++;
		}
		assert(count == ITEMS_PER_EPOCH);
	}
	printf("PASS: every record in every epoch delivered exactly once, no drops/duplicates\n");

	printf("\nALL TESTS PASSED\n");
	rte_eal_cleanup();
	return 0;
}
