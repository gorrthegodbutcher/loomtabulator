/* Plugin ABI shim for the built-in "convert" stage - see
 * validate_stage_plugin.c's own header comment for the general shape
 * every one of these shims follows. */
#include "convert_stage.h"
#include "../stage_abi.h"

static const struct stage g_stage = {
	.name = "convert",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_EXTRACTED),
	.out_type = PORT_TYPE_ENGINEERING,
	.init = convert_stage_init,
	.process = convert_stage_process,
	.teardown = convert_stage_teardown,
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
