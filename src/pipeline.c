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

	for (size_t i = 0; i < chain->stage_count; i++) {
		const struct pipeline_stage_instance *inst = &chain->stages[i];
		struct stage_record next = {
			.data = worker->scratch[(i + 1) % 2],
		};

		struct stage_result res = inst->stage->process(inst->state, &cur, &next);
		if (!res.ok) {
			atomic_fetch_add_explicit(&counters->records_dropped, 1, memory_order_relaxed);
			fprintf(stderr, "loomtabulator: dropped at stage '%s': %s\n",
				inst->stage->name, res.drop_reason ? res.drop_reason : "(no reason given)");
			return false;
		}
		cur = next;
	}

	atomic_fetch_add_explicit(&counters->records_forwarded, 1, memory_order_relaxed);
	return true;
}
