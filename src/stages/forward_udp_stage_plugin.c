/* Plugin ABI shim for the built-in "forward_udp" stage - see
 * validate_stage_plugin.c's own header comment for the general shape
 * every one of these shims follows. */
#include "forward_udp_stage.h"
#include "../stage_abi.h"

static const struct stage g_stage = {
	.name = "forward_udp",
	.in_type = PORT_TYPE_ENGINEERING,
	.out_type = PORT_TYPE_WIRE_FRAME,
	.max_out_ports = 1,
	.init = forward_udp_stage_init,
	.process = forward_udp_stage_process,
	.teardown = forward_udp_stage_teardown,
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
