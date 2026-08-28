#include <string.h>
#include <stdio.h>
#include "pipeline.h"

bool
pipeline_run(struct pipeline *pl, const uint8_t *raw_data, uint32_t raw_len, uint64_t capture_tsc)
{
	pl->records_in++;

	struct stage_record cur = {
		.type = PORT_TYPE_RAW_RECORD,
		.data = pl->scratch[0],
		.len = raw_len,
		.capture_tsc = capture_tsc,
	};
	memcpy(pl->scratch[0], raw_data, raw_len);

	for (size_t i = 0; i < pl->stage_count; i++) {
		struct pipeline_stage_instance *inst = &pl->stages[i];
		struct stage_record next = {
			.data = pl->scratch[(i + 1) % 2],
		};

		struct stage_result res = inst->stage->process(inst->state, &cur, &next);
		if (!res.ok) {
			pl->records_dropped++;
			fprintf(stderr, "loomtabulator: dropped at stage '%s': %s\n",
				inst->stage->name, res.drop_reason ? res.drop_reason : "(no reason given)");
			return false;
		}
		cur = next;
	}

	pl->records_forwarded++;
	return true;
}
