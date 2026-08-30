#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdatomic.h>
#include "stage.h"

/* Total graph nodes, not "chain length" - the graph is a tree now (see
 * graph_config.c), not a flat linear chain, but this cap is still just
 * generous headroom, not a real architectural limit. */
#define PIPELINE_MAX_STAGES 16

struct pipeline_stage_instance {
	const struct stage *stage; /* points into plugin_loader.c's
				      * dynamically-populated table - never
				      * owned/freed here */
	void *state;                /* this instance's own init() result */

	/* This node's own out_port_count(state) result, resolved once by
	 * graph_config_load() and cached here - pipeline_run() never calls
	 * out_port_count() again on the hot path. 0 means this is a leaf
	 * (graph_config.c already confirmed stage->out_type ==
	 * PORT_TYPE_WIRE_FRAME for every leaf). */
	unsigned port_count;

	/* children[k] is the chain->stages[] index that output port k
	 * routes to - always valid (>= 0) for k < port_count, since
	 * graph_config.c guarantees every declared port is wired to a real
	 * edge; -1 for k >= port_count (structurally unused). */
	int children[STAGE_MAX_OUT_PORTS];
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
	size_t root_idx; /* chain->stages[] index of the tree's single root -
			    * pipeline_run()'s walk always starts here. */
};

/* Per-worker. MUST NOT be shared or aliased across lcores - concurrent
 * workers writing through the same scratch buffer would corrupt each
 * other's in-flight records. One instance per worker lcore, owned
 * entirely by that worker for its whole lifetime (see
 * pipeline_worker.c). */
struct pipeline_worker {
	/* Ping-pong scratch buffers - keyed on a record's depth along
	 * whichever root-to-leaf path it takes through the tree (depth 0
	 * reads from scratch[0], writes to scratch[1]; depth 1 reads
	 * scratch[1], writes scratch[0]; etc - see pipeline.c), not on
	 * array position, since chain->stages[] is no longer walked in a
	 * fixed linear order. Still exactly 2 buffers regardless of tree
	 * shape or depth: a routing decision picks exactly one child per
	 * record (no broadcast/fan-out to multiple children at once - see
	 * stage.h's out_port_count comment), so exactly one stage is ever
	 * "current" at a time within a single pipeline_run() call, same as
	 * before. Every stage always fully copies its output (see e.g.
	 * validate_stage.c's own comment) rather than aliasing the input
	 * buffer, so there's never a case where a stage needs to read and
	 * write the same buffer at once. Concurrency comes from multiple
	 * workers each running their own pipeline_run() call against their
	 * own pipeline_worker, not from within one call. */
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
 * through the chain starting at chain->root_idx, following whichever
 * single child each stage's stage_result.out_port selects until it
 * reaches a leaf (port_count == 0), using worker's own scratch buffers
 * (never touched by any other concurrently-running pipeline_run() call)
 * and counters (shared, atomic - safe to update from any number of
 * concurrent callers). Returns true if it reached a leaf with every
 * stage along the way reporting ok=true (a leaf always produces a wire
 * frame - see stage.h's out_port_count comment - so this means the
 * packet was actually transmitted); false if any stage dropped it, or
 * if a stage returned an out_port outside its own declared range (a
 * plugin bug, logged distinctly from an ordinary drop - see
 * pipeline.c). The specific stage and reason are logged at the point of
 * the drop (see pipeline.c) - pipeline_run()'s return value itself is
 * deliberately just pass/fail, not "which stage." */
bool pipeline_run(const struct pipeline_chain *chain, struct pipeline_worker *worker,
		   struct pipeline_counters *counters, const uint8_t *raw_data,
		   uint32_t raw_len, uint64_t capture_tsc);

#endif
