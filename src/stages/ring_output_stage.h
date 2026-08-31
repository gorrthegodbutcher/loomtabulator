#ifndef RING_OUTPUT_STAGE_H
#define RING_OUTPUT_STAGE_H

#include "../stage.h"

void *ring_output_stage_init(const struct json_value *config);
struct stage_result ring_output_stage_process(void *state, const struct stage_record *in,
					       struct stage_record *out);
void ring_output_stage_teardown(void *state);
void ring_output_stage_get_status(void *state, struct stage_status *out);

#endif
