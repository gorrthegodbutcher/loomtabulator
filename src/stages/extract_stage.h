#ifndef EXTRACT_STAGE_H
#define EXTRACT_STAGE_H

#include "../stage.h"

void *extract_stage_init(const struct json_value *config);
struct stage_result extract_stage_process(void *state, const struct stage_record *in,
					   struct stage_record *out);
void extract_stage_teardown(void *state);
void extract_stage_get_config_schema(struct stage_config_schema *out);

#endif
