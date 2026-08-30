/* Half of a collision pair (with plugin_dup_b.c) - both declare the
 * same stage name, built into a separate fixture directory from the
 * other fixtures so loading THIS directory specifically exercises
 * plugin_loader.c's name-collision rejection (fatal - returns false). */
#include "../../src/stage_abi.h"

static struct stage_result
fixture_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	(void)state; (void)in; (void)out;
	return (struct stage_result){ .ok = true };
}

static const struct stage g_stage = {
	.name = "fixture_dup",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD),
	.out_type = PORT_TYPE_VALIDATED,
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
