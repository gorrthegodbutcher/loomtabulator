#ifndef VALIDATE_STAGE_H
#define VALIDATE_STAGE_H

#include "../stage.h"

void *validate_stage_init(const struct json_value *config);
struct stage_result validate_stage_process(void *state, const struct stage_record *in,
					    struct stage_record *out);
void validate_stage_teardown(void *state);
void validate_stage_get_status(void *state, struct stage_status *out);

#endif
