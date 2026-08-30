/* Fixture plugin that forgets to export loom_stage_abi_version() -
 * exercises plugin_loader.c's missing-symbol rejection path (the exact
 * "forgot to export the version symbol" mistake a plugin author could
 * make - should be logged and skipped, NOT fatal). Deliberately does
 * NOT include stage_abi.h's version macro/typedefs to make the
 * omission obvious in the source itself. */
#include "../../src/stage.h"

static struct stage_result
fixture_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	(void)state; (void)in; (void)out;
	return (struct stage_result){ .ok = true };
}

static const struct stage g_stage = {
	.name = "fixture_missingversion",
	.in_type = PORT_TYPE_RAW_RECORD,
	.out_type = PORT_TYPE_VALIDATED,
	.init = NULL,
	.process = fixture_process,
	.teardown = NULL,
};

/* No loom_stage_abi_version() export - that's the point of this fixture. */
const struct stage *
loom_stage_entry(void)
{
	return &g_stage;
}
