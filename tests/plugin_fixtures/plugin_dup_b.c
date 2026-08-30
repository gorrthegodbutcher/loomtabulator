/* Other half of the collision pair - see plugin_dup_a.c. Same stage
 * name on purpose. */
#include "../../src/stage_abi.h"

static struct stage_result
fixture_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	(void)state; (void)in; (void)out;
	return (struct stage_result){ .ok = true };
}

static const struct stage g_stage = {
	.name = "fixture_dup",
	.in_type = PORT_TYPE_VALIDATED,
	.out_type = PORT_TYPE_EXTRACTED,
	.init = NULL,
	.process = fixture_process,
	.teardown = NULL,
};

uint32_t
loom_stage_abi_version(void)
{
	return STAGE_ABI_VERSION;
}

const struct stage *
loom_stage_entry(void)
{
	return &g_stage;
}
