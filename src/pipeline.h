#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdatomic.h>
#include "stage.h"

/* v1 is a linear chain only (see graph_config.c's own validation) - this
 * cap is generous headroom for that, not a real architectural limit.
 * A future branching/fan-out pipeline will need a real graph structure
 * here instead of a flat array; not needed yet. */
#define PIPELINE_MAX_STAGES 16

struct pipeline_stage_instance {
	const struct stage *stage; /* points into stage_registry.c's static
				      * table - never owned/freed here */
	void *state;                /* this instance's own init() result */
};

/* Shared, read-only after graph_config_load() builds it once at startup.
 * Every v1 stage's own process() only ever reads its init()-time state
 * (e.g. convert_stage.c's scale/offset coefficients are never mutated),
 * so sharing this struct read-only across every worker lcore, with no
 * lock, is safe - see the project plan's Phase 2 section. Split out of
 * what used to be a single `struct pipeline` specifically so this part
 * (the chain shape) and the per-worker part below (scratch memory) have
 * independent lifetimes: one instance of this, one instance of
 * pipeline_worker per worker lcore. */
struct pipeline_chain {
	struct pipeline_stage_instance stages[PIPELINE_MAX_STAGES];
	size_t stage_count;
};

/* Per-worker. MUST NOT be shared or aliased across lcores - concurrent
 * workers writing through the same scratch buffer would corrupt each
 * other's in-flight records. One instance per worker lcore, owned
 * entirely by that worker for its whole lifetime (see
 * pipeline_worker.c). */
struct pipeline_worker {
	/* Ping-pong scratch buffers - stage i reads from scratch[i % 2],
	 * writes to scratch[(i + 1) % 2]. Every stage always fully copies
	 * its output (see e.g. validate_stage.c's own comment) rather than
	 * aliasing the input buffer, so there's never a case where a stage
	 * needs to read and write the same buffer at once. Two buffers is
	 * enough regardless of chain length since only one stage ever runs
	 * at a time within a single pipeline_run() call - concurrency comes
	 * from multiple workers each running their own pipeline_run() call
	 * against their own pipeline_worker, not from within one call. */
	uint8_t scratch[2][STAGE_SCRATCH_BYTES];
};

/* Plain atomics, not per-worker-then-summed - matches
 * dpdk-app-example's web_status.c's own atomic-counter convention.
 * memory_order_relaxed throughout: these are stats only, nothing
 * downstream depends on their ordering relative to other memory
 * operations. */
struct pipeline_counters {
	atomic_uint_least64_t records_in;
	atomic_uint_least64_t records_dropped;
	atomic_uint_least64_t records_forwarded;
};

void pipeline_counters_init(struct pipeline_counters *pc);

/* Runs one raw record (as read off the input ring - see ring_input.h)
 * through every stage in chain in order, using worker's own scratch
 * buffers (never touched by any other concurrently-running
 * pipeline_run() call) and counters (shared, atomic - safe to update
 * from any number of concurrent callers). Returns true if it reached
 * the end of the chain with every stage reporting ok=true (for a v1
 * graph ending in forward_udp, that means the packet was actually
 * transmitted); false if any stage dropped it. The specific stage and
 * drop_reason are logged at the point of the drop (see pipeline.c) -
 * pipeline_run()'s return value itself is deliberately just pass/fail,
 * not "which stage." */
bool pipeline_run(const struct pipeline_chain *chain, struct pipeline_worker *worker,
		   struct pipeline_counters *counters, const uint8_t *raw_data,
		   uint32_t raw_len, uint64_t capture_tsc);

#endif
