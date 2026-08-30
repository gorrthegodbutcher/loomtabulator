/* Fixture plugin with a deliberately wrong ABI version - exercises
 * plugin_loader.c's version-mismatch rejection path (should be logged
 * and skipped, NOT fatal to the whole scan). */
#include "../../src/stage_abi.h"

static struct stage_result
fixture_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	(void)state; (void)in; (void)out;
	return (struct stage_result){ .ok = true };
}

static const struct stage g_stage = {
	.name = "fixture_badversion",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD),
	.out_type = PORT_TYPE_RAW_RECORD,
	.init = NULL,
	.process = fixture_process,
	.teardown = NULL,
};

uint32_t
loom_stage_abi_version(void)
{
	return STAGE_ABI_VERSION + 1000u; /* deliberately wrong */
}

const struct stage *
loom_stage_entry(void)
{
	return &g_stage;
}
