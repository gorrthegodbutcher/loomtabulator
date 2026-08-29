#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include "../src/epoch_barrier.h"

/* DPDK-free, real pthreads - see the project plan's "Phase 2" section for
 * the full design rationale (~/.claude/plans/noble-kindling-lemon.md).
 *
 * This test validates epoch_barrier.c's own state machine in isolation,
 * NOT a real rte_ring - a fixed in-memory "script" array, claimed via a
 * shared atomic_fetch_add index, stands in for the ring. That claiming
 * pattern *is* the same FIFO-across-consumers guarantee a real
 * multi-consumer rte_ring provides (whoever claims index i is guaranteed
 * no one else claims an index < i after them) - see
 * tests/test_pipeline_workers.c for the equivalent test against a real
 * rte_ring, which this one deliberately doesn't need. */

#define NUM_EPOCHS 6
#define ITEMS_PER_EPOCH 200
#define NUM_WORKERS 4

enum item_kind { ITEM_DATA, ITEM_BARRIER };

struct script_item {
	enum item_kind kind;
	uint64_t seq;   /* DATA: per-epoch sequence number. BARRIER: epoch id. */
	uint64_t epoch; /* DATA only: which epoch this item was scripted into. */
};

static struct script_item g_script[NUM_EPOCHS * (ITEMS_PER_EPOCH + 1)];
static size_t g_script_len;
static _Atomic size_t g_next_idx;

struct data_result {
	bool used;
	uint64_t scripted_epoch;
	uint64_t entered_epoch;
	uint64_t entry_ns;
	uint64_t exit_ns;
	int worker_idx;
};

static struct data_result g_results[NUM_EPOCHS * ITEMS_PER_EPOCH];
static _Atomic size_t g_results_next;

struct barrier_result {
	uint64_t barrier_seq;
	uint64_t new_epoch;
};

static struct barrier_result g_barrier_results[NUM_EPOCHS];
static _Atomic size_t g_barrier_results_next;

static struct epoch_barrier g_barrier;

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void
build_script(void)
{
	size_t idx = 0;
	for (uint64_t e = 0; e < NUM_EPOCHS; e++) {
		for (uint64_t s = 0; s < ITEMS_PER_EPOCH; s++)
			g_script[idx++] = (struct script_item){ .kind = ITEM_DATA, .seq = s, .epoch = e };
		g_script[idx++] = (struct script_item){ .kind = ITEM_BARRIER, .seq = e };
	}
	g_script_len = idx;
	assert(g_script_len == NUM_EPOCHS * (ITEMS_PER_EPOCH + 1));
}

static void *
worker_main(void *arg)
{
	int worker_idx = *(int *)arg;

	for (;;) {
		epoch_barrier_wait_if_pending(&g_barrier);

		size_t idx = atomic_fetch_add(&g_next_idx, 1);
		if (idx >= g_script_len)
			break;
		struct script_item *item = &g_script[idx];

		if (item->kind == ITEM_BARRIER) {
			uint64_t new_epoch = epoch_barrier_drain(&g_barrier, item->seq, 0);
			size_t r = atomic_fetch_add(&g_barrier_results_next, 1);
			g_barrier_results[r] = (struct barrier_result){
				.barrier_seq = item->seq,
				.new_epoch = new_epoch,
			};
		} else {
			uint64_t entered_epoch = epoch_barrier_enter(&g_barrier);
			uint64_t entry_ns = now_ns();

			/* Small randomized delay so worker threads genuinely
			 * interleave in wallclock time within an epoch, instead
			 * of accidentally serializing - see the concurrency
			 * sanity check below. */
			struct timespec req = { .tv_sec = 0, .tv_nsec = (rand() % 50) * 1000 };
			nanosleep(&req, NULL);

			uint64_t exit_ns = now_ns();
			epoch_barrier_exit(&g_barrier);

			size_t r = atomic_fetch_add(&g_results_next, 1);
			g_results[r] = (struct data_result){
				.used = true,
				.scripted_epoch = item->epoch,
				.entered_epoch = entered_epoch,
				.entry_ns = entry_ns,
				.exit_ns = exit_ns,
				.worker_idx = worker_idx,
			};
		}
	}

	return NULL;
}

int
main(void)
{
	build_script();
	epoch_barrier_init(&g_barrier);

	pthread_t threads[NUM_WORKERS];
	int worker_ids[NUM_WORKERS];
	for (int i = 0; i < NUM_WORKERS; i++) {
		worker_ids[i] = i;
		assert(pthread_create(&threads[i], NULL, worker_main, &worker_ids[i]) == 0);
	}
	for (int i = 0; i < NUM_WORKERS; i++)
		pthread_join(threads[i], NULL);

	size_t n_results = atomic_load(&g_results_next);
	size_t n_barriers = atomic_load(&g_barrier_results_next);
	assert(n_results == NUM_EPOCHS * ITEMS_PER_EPOCH);
	assert(n_barriers == NUM_EPOCHS);
	printf("PASS: every scripted data/barrier item was processed exactly once "
	       "(%zu data, %zu barriers)\n", n_results, n_barriers);

	/* Every data item's epoch (as returned by enter()) matches its
	 * scripted epoch - this is exactly the race the rejected
	 * per-epoch-ping-pong-counter design (see epoch_barrier.h) would
	 * fail. */
	for (size_t i = 0; i < n_results; i++)
		assert(g_results[i].entered_epoch == g_results[i].scripted_epoch);
	printf("PASS: every data item was tagged with its correct scripted epoch\n");

	/* Each drained barrier's own seq (the epoch it closes) is exactly
	 * one less than the epoch_barrier's new_epoch after that drain -
	 * the same cross-check pipeline_worker.c makes against a barrier
	 * record's hdr->seq. */
	for (size_t i = 0; i < n_barriers; i++)
		assert(g_barrier_results[i].barrier_seq + 1 == g_barrier_results[i].new_epoch);
	printf("PASS: every drained barrier's seq matches the epoch it closed\n");

	/* The actual ordering guarantee: for every pair of adjacent epochs,
	 * every completion (exit_ns) in epoch K precedes every entry
	 * (entry_ns) in epoch K+1 - a hard cliff at each boundary, checked
	 * directly from wallclock timestamps rather than trusted by
	 * construction. */
	for (uint64_t e = 0; e + 1 < NUM_EPOCHS; e++) {
		uint64_t max_exit_this = 0, min_entry_next = UINT64_MAX;
		for (size_t i = 0; i < n_results; i++) {
			if (g_results[i].scripted_epoch == e && g_results[i].exit_ns > max_exit_this)
				max_exit_this = g_results[i].exit_ns;
			if (g_results[i].scripted_epoch == e + 1 && g_results[i].entry_ns < min_entry_next)
				min_entry_next = g_results[i].entry_ns;
		}
		assert(max_exit_this < min_entry_next);
	}
	printf("PASS: zero interleaving across every epoch boundary (%d epochs checked)\n",
	       NUM_EPOCHS - 1);

	/* Concurrency sanity check: within a single epoch, at least two
	 * different worker threads' entry/exit intervals should overlap in
	 * wallclock time - if this ever fails, the test's own concurrency
	 * (worker count, artificial jitter) needs tuning, not a sign the
	 * barrier logic itself is broken. */
	bool saw_overlap = false;
	for (size_t i = 0; i < n_results && !saw_overlap; i++) {
		for (size_t j = i + 1; j < n_results; j++) {
			if (g_results[i].scripted_epoch != g_results[j].scripted_epoch)
				continue;
			if (g_results[i].worker_idx == g_results[j].worker_idx)
				continue;
			bool overlap = g_results[i].entry_ns < g_results[j].exit_ns &&
					g_results[j].entry_ns < g_results[i].exit_ns;
			if (overlap) {
				saw_overlap = true;
				break;
			}
		}
	}
	assert(saw_overlap);
	printf("PASS: worker threads genuinely interleaved within an epoch (test is "
	       "actually exercising concurrency)\n");

	printf("\nALL TESTS PASSED\n");
	return 0;
}
