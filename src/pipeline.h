#ifndef PIPELINE_H
#define PIPELINE_H

#include "stage.h"

/* v1 is a linear chain only (see graph_config.c's own validation) - this
 * cap is generous headroom for that, not a real architectural limit.
 * Phase 2's branching/fan-out pipelines will need a real graph structure
 * here instead of a flat array; not needed yet. */
#define PIPELINE_MAX_STAGES 16

struct pipeline_stage_instance {
	const struct stage *stage; /* points into stage_registry.c's static
				      * table - never owned/freed here */
	void *state;                /* this instance's own init() result */
};

struct pipeline {
	struct pipeline_stage_instance stages[PIPELINE_MAX_STAGES];
	size_t stage_count;

	/* Ping-pong scratch buffers - stage i reads from scratch[i % 2],
	 * writes to scratch[(i + 1) % 2]. Every stage always fully copies
	 * its output (see e.g. validate_stage.c's own comment) rather than
	 * aliasing the input buffer, so there's never a case where a stage
	 * needs to read and write the same buffer at once. Two buffers is
	 * enough regardless of chain length since only one stage ever runs
	 * at a time in v1's single-core, run-to-completion model - see the
	 * project plan's Phase 2 notes for what changes once that's no
	 * longer true. */
	uint8_t scratch[2][STAGE_SCRATCH_BYTES];

	uint64_t records_in;
	uint64_t records_dropped;
	uint64_t records_forwarded;
};

/* Runs one raw record (as read off the input ring - see ring_input.h)
 * through every stage in order. Returns true if it reached the end of
 * the chain with every stage reporting ok=true (for a v1 graph ending in
 * forward_udp, that means the packet was actually transmitted); false if
 * any stage dropped it. The specific stage and drop_reason are logged at
 * the point of the drop (see pipeline.c) - pipeline_run()'s return value
 * itself is deliberately just pass/fail, not "which stage." */
bool pipeline_run(struct pipeline *pl, const uint8_t *raw_data, uint32_t raw_len,
		   uint64_t capture_tsc);

#endif
