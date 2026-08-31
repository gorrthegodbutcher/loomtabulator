/* Plugin ABI shim for the built-in "null_sink" stage - see
 * validate_stage_plugin.c's own header comment for the general shape
 * every one of these shims follows. */
#include "null_sink_stage.h"
#include "../stage_abi.h"

/* A genuine leaf, same reasoning as dump_binary_stage_plugin.c's own
 * out_port_count - it discards the record and has nothing further to
 * route to. */
static unsigned
null_sink_out_port_count(void *state)
{
	(void)state;
	return 0;
}

static const struct stage g_stage = {
	.name = "null_sink",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD) | PORT_TYPE_BIT(PORT_TYPE_ENGINEERING) |
		    PORT_TYPE_BIT(PORT_TYPE_WIRE_FRAME), /* genuinely everything - see
							     null_sink_stage.c's own comment */
	.out_type = PORT_TYPE_RAW_RECORD, /* unused for a leaf - see stage.h's
					      out_port_count comment */
	.init = null_sink_stage_init,
	.out_port_count = null_sink_out_port_count,
	.process = null_sink_stage_process,
	.teardown = null_sink_stage_teardown,
	.get_status = null_sink_stage_get_status,
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
