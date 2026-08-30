/* A minimal, fully valid fixture plugin for tests/test_plugin_loader.c -
 * exercises the success path. init/teardown are NULL (valid per
 * stage.h's own "not every stage needs teardown" comment); process()
 * is a trivial stub since this test never runs data through it, only
 * exercises plugin_loader.c's load/register logic. */
#include "../../src/stage_abi.h"

static struct stage_result
fixture_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	(void)state; (void)in; (void)out;
	return (struct stage_result){ .ok = true };
}

static const struct stage g_stage = {
	.name = "fixture_ok",
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
