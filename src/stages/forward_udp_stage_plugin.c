/* Plugin ABI shim for the built-in "forward_udp" stage - see
 * validate_stage_plugin.c's own header comment for the general shape
 * every one of these shims follows. */
#include "forward_udp_stage.h"
#include "../stage_abi.h"

/* forward_udp is a genuine leaf - it transmits and has nothing further
 * to route to, unlike validate/extract/convert (which rely on
 * out_port_count's NULL default of "1 port"). Declaring 0 here is what
 * lets graph_config.c tell "the end of the chain" apart from "a
 * single-output stage whose one edge just hasn't been wired yet" -
 * see stage.h's out_port_count comment. */
static unsigned
forward_udp_out_port_count(void *state)
{
	(void)state;
	return 0;
}

static const struct stage g_stage = {
	.name = "forward_udp",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD),
	.out_type = PORT_TYPE_WIRE_FRAME,
	.init = forward_udp_stage_init,
	.out_port_count = forward_udp_out_port_count,
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
