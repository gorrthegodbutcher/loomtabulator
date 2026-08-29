#ifndef PIPELINE_WORKER_H
#define PIPELINE_WORKER_H

#include <stdbool.h>
#include <rte_ring.h>
#include "pipeline.h"
#include "epoch_barrier.h"

/* One instance per worker lcore - see main.c's launch loop. chain/
 * counters/barrier/ring/stop_requested are all shared across every
 * worker (chain is read-only after graph_config_load(); counters and
 * barrier are internally synchronized; ring is DPDK's own
 * multi-consumer-safe rte_ring); worker is this instance's own scratch
 * memory, never touched by any other worker - see pipeline.h's own
 * header comment on why that split exists. */
struct pipeline_worker_ctx {
	struct rte_ring *ring;
	const struct pipeline_chain *chain;
	struct pipeline_worker worker;
	struct pipeline_counters *counters;
	struct epoch_barrier *barrier;
	const volatile bool *stop_requested;
	unsigned int worker_id; /* for logging only */

	/* Optional. Called right after a data record's pipeline_run() call
	 * returns (regardless of ok/drop - "completed" means "this record's
	 * processing for its epoch is done", not specifically "forwarded"),
	 * with the epoch epoch_barrier_enter() returned for it and the
	 * record's own hdr->seq. NULL = no-op. Exists purely as a
	 * verification/debug hook - see tests/test_pipeline_workers.c,
	 * which uses it to log completion order without needing to touch
	 * pipeline_run()'s own signature. A future epoch-aware output stage
	 * could reuse the same hook point instead of a new one. */
	void (*on_record_processed)(void *cb_arg, uint64_t epoch, uint64_t seq);
	void *cb_arg;
};

/* rte_eal_remote_launch()-compatible entry point (int(*)(void*), arg is
 * a struct pipeline_worker_ctx*). Also callable directly from a plain
 * pthread - its own body only ever touches rte_ring/atomics, no
 * lcore-specific API - see tests/test_pipeline_workers.c, which does
 * exactly that to exercise this function without needing multiple EAL
 * lcores reserved.
 *
 * Loop shape: wait_if_pending() -> dequeue one item -> if it's a barrier
 * record, epoch_barrier_drain(); otherwise epoch_barrier_enter() ->
 * pipeline_run() -> epoch_barrier_exit(). Checks *stop_requested only at
 * the top of the outer loop, never mid-drain, so a worker that's
 * actively draining a barrier always finishes that drain before
 * noticing shutdown - same "never leave the barrier in a half-drained
 * state" reasoning as the project plan's Phase 2 section. */
int pipeline_worker_lcore_main(void *arg);

#endif
