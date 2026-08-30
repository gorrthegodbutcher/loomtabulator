/* The plugin ABI shim for the built-in "validate" stage - see
 * ../stage_abi.h for the two-export contract every plugin (built-in or
 * third-party) satisfies. This file is the ENTIRE difference between
 * "validate_stage.c compiled into the host binary" (the pre-plugin
 * design) and "validate_stage.c compiled into its own loadable .so" -
 * the stage logic itself (validate_stage.c) is completely unaware
 * it's being loaded dynamically. */
#include "validate_stage.h"
#include "../stage_abi.h"

static const struct stage g_stage = {
	.name = "validate",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD),
	.out_type = PORT_TYPE_VALIDATED,
	.init = validate_stage_init,
	.process = validate_stage_process,
	.teardown = validate_stage_teardown,
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
