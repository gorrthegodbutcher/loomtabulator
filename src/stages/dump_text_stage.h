#ifndef DUMP_TEXT_STAGE_H
#define DUMP_TEXT_STAGE_H

#include "../stage.h"

void *dump_text_stage_init(const struct json_value *config);
struct stage_result dump_text_stage_process(void *state, const struct stage_record *in,
					     struct stage_record *out);
void dump_text_stage_teardown(void *state);

#endif
