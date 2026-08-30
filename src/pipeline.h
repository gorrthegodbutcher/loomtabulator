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

	/* This node's own "id" string straight out of the graph JSON (see
	 * graph_config.c) - points into the same long-lived parsed-JSON
	 * buffer every other string this struct's stage points to already
	 * relies on staying alive (graph_config.c's own read_whole_file()
	 * is "intentionally never freed"). Exists purely so web_status.c's
	 * periodic status collection (struct stage.get_status, stage.h) can
	 * key its results by the exact same id the web UI's own graph nodes
	 * use - never read on the hot path. */
	const char *node_id;

	/* This node's own out_port_count(state) result, resolved once by
	 * graph_config_load() and cached here - pipeline_run() never calls
	 * out_port_count() again on the hot path. 0 means this is a leaf -
	 * out_type is unused/unconstrained for one (see stage.h's
	 * out_port_count comment; there's more than one kind of terminal
	 * sink since version 4, so "produces wire_frame" stopped being the
	 * leaf marker). */
	unsigned port_count;

	/* children[k] is the chain->stages[] index that output port k
	 * routes to - always valid (>= 0) for k < port_count, since
	 * graph_config.c guarantees every declared port is wired to a real
	 * edge; -1 for k >= port_count (structurally unused). */
	int children[STAGE_MAX_OUT_PORTS];

	/* Engine-level routing for a record this stage flagged as invalid
	 * (STAGE_RECORD_FLAG_INTEGRITY_FAILED - see stage.h) - orthogonal to
	 * children[]/port_count above, and to whatever out_port a flagged
	 * record's stage_result carries (a flagged record never consults
	 * out_port at all - see pipeline.c). Resolved once by
	 * graph_config_load() from this node's optional "on_invalid" graph
	 * property and its at-most-one edge marked "invalid_target" (see
	 * graph_config.c) - the stage itself never declares or knows about
	 * this, which is the whole point: any stage, built-in or
	 * third-party, gets flagged-record routing for free just by setting
	 * the flag bit.
	 *
	 * invalid_child: chain->stages[] index a flagged record is routed to
	 * instead of its normal child, or -1 if this node has no dedicated
	 * invalid-record edge. When set, this always wins over pass_invalid
	 * below.
	 *
	 * pass_invalid: when invalid_child == -1, whether a flagged record
	 * still continues down its normal out_port (true, "on_invalid":
	 * "pass") or is dropped right here, counted the same as an ok=false
	 * result (false, "on_invalid": "drop" - the default, matching every
	 * stage's pre-version-5 hard-drop-on-failure behavior). */
	int invalid_child;
	bool pass_invalid;
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
 * single child each stage's stage_result.out_port selects, until it
 * reaches a leaf (port_count == 0) - unless a stage flags a record
 * invalid (STAGE_RECORD_FLAG_INTEGRITY_FAILED), in which case out_port
 * is never consulted at all: the record instead goes to that node's
 * invalid_child if one is wired, or is dropped/passed through per its
 * pass_invalid setting (see pipeline_stage_instance above) - this
 * applies at every node along the path, leaf or not, so a leaf can
 * still redirect its own flagged records to a different final sink.
 * Uses worker's own scratch buffers (never touched by any other
 * concurrently-running pipeline_run() call) and counters (shared,
 * atomic - safe to update from any number of concurrent callers).
 * Returns true if it reached a leaf with every stage along the way
 * reporting ok=true (and no unresolved invalid-drop along the way);
 * false if any stage dropped it (ok=false, or ok=true but flagged
 * invalid with nowhere configured to route it), or if a stage returned
 * an out_port outside its own declared range (a plugin bug, logged
 * distinctly from an ordinary drop - see pipeline.c). The specific
 * stage and reason are logged at the point of the drop (see
 * pipeline.c) - pipeline_run()'s return value itself is deliberately
 * just pass/fail, not "which stage." */
bool pipeline_run(const struct pipeline_chain *chain, struct pipeline_worker *worker,
		   struct pipeline_counters *counters, const uint8_t *raw_data,
		   uint32_t raw_len, uint64_t capture_tsc);

#endif
