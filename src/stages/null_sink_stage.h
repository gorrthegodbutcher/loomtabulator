#ifndef NULL_SINK_STAGE_H
#define NULL_SINK_STAGE_H

#include "../stage.h"

void *null_sink_stage_init(const struct json_value *config);
struct stage_result null_sink_stage_process(void *state, const struct stage_record *in,
					     struct stage_record *out);
void null_sink_stage_teardown(void *state);
void null_sink_stage_get_status(void *state, struct stage_status *out);

#endif
