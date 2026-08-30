#include <string.h>
#include <stdio.h>
#include "pipeline.h"

void
pipeline_counters_init(struct pipeline_counters *pc)
{
	atomic_store(&pc->records_in, 0);
	atomic_store(&pc->records_dropped, 0);
	atomic_store(&pc->records_forwarded, 0);
}

bool
pipeline_run(const struct pipeline_chain *chain, struct pipeline_worker *worker,
	     struct pipeline_counters *counters, const uint8_t *raw_data, uint32_t raw_len,
	     uint64_t capture_tsc)
{
	atomic_fetch_add_explicit(&counters->records_in, 1, memory_order_relaxed);

	struct stage_record cur = {
		.type = PORT_TYPE_RAW_RECORD,
		.data = worker->scratch[0],
		.len = raw_len,
		.capture_tsc = capture_tsc,
	};
	memcpy(worker->scratch[0], raw_data, raw_len);

	size_t idx = chain->root_idx;
	unsigned depth = 0;
	for (;;) {
		const struct pipeline_stage_instance *inst = &chain->stages[idx];
		/* Deliberate, ABI-level guarantee (see stage.h's own comment
		 * on struct stage.process): every field left off this
		 * designated initializer - .type/.len/.capture_tsc/.flags -
		 * is zeroed before process() ever sees it, so a stage that
		 * doesn't care about e.g. STAGE_RECORD_FLAG_* can leave
		 * out->flags untouched rather than needing an explicit
		 * `.flags = 0`. Don't replace this with something that skips
		 * the zero-fill (e.g. reusing a stale buffer via memcpy)
		 * without updating that comment too. */
		struct stage_record next = {
			.data = worker->scratch[(depth + 1) % 2],
		};

		struct stage_result res = inst->stage->process(inst->state, &cur, &next);
		if (!res.ok) {
			atomic_fetch_add_explicit(&counters->records_dropped, 1, memory_order_relaxed);
			fprintf(stderr, "loomtabulator: dropped at stage '%s': %s\n",
				inst->stage->name, res.drop_reason ? res.drop_reason : "(no reason given)");
			return false;
		}

		/* Leaf - graph_config.c already proved out_type ==
		 * PORT_TYPE_WIRE_FRAME for every node with port_count == 0,
		 * so reaching one here means the record was fully processed. */
		if (inst->port_count == 0)
			break;

		if (res.out_port >= inst->port_count) {
			/* A plugin returning an out_port outside its own
			 * declared range is a runtime contract violation, not
			 * an ordinary drop (graph_config.c already guaranteed
			 * every value in [0, port_count) IS wired) - logged
			 * distinctly so it reads as a plugin bug, not routing
			 * policy. */
			atomic_fetch_add_explicit(&counters->records_dropped, 1, memory_order_relaxed);
			fprintf(stderr,
				"loomtabulator: stage '%s' returned out-of-range out_port %u "
				"(declares %u port(s)) - dropping record\n",
				inst->stage->name, res.out_port, inst->port_count);
			return false;
		}

		idx = (size_t)inst->children[res.out_port];
		cur = next;
		depth++;
	}

	atomic_fetch_add_explicit(&counters->records_forwarded, 1, memory_order_relaxed);
	return true;
}
