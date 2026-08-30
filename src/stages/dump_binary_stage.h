#ifndef DUMP_BINARY_STAGE_H
#define DUMP_BINARY_STAGE_H

#include "../stage.h"

void *dump_binary_stage_init(const struct json_value *config);
struct stage_result dump_binary_stage_process(void *state, const struct stage_record *in,
					       struct stage_record *out);
void dump_binary_stage_teardown(void *state);
void dump_binary_stage_get_status(void *state, struct stage_status *out);

#endif
