#ifndef BYTE_MATCH_STAGE_H
#define BYTE_MATCH_STAGE_H

#include "../stage.h"

void *byte_match_stage_init(const struct json_value *config);
struct stage_result byte_match_stage_process(void *state, const struct stage_record *in,
					      struct stage_record *out);
void byte_match_stage_teardown(void *state);
void byte_match_stage_get_status(void *state, struct stage_status *out);
void byte_match_stage_get_config_schema(struct stage_config_schema *out);

#endif
