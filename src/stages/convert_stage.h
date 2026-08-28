#ifndef CONVERT_STAGE_H
#define CONVERT_STAGE_H

#include "../stage.h"

void *convert_stage_init(const struct json_value *config);
struct stage_result convert_stage_process(void *state, const struct stage_record *in,
					   struct stage_record *out);
void convert_stage_teardown(void *state);

#endif
