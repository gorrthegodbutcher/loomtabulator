/* Plugin ABI shim for the built-in "ring_output" stage - see
 * validate_stage_plugin.c's own header comment for the general shape
 * every one of these shims follows. */
#include "ring_output_stage.h"
#include "../stage_abi.h"

/* A genuine leaf, same reasoning as dump_binary_stage_plugin.c's own
 * out_port_count - it enqueues onto an rte_ring for a completely
 * separate process to consume and has nothing further to route to
 * within this graph. */
static unsigned
ring_output_out_port_count(void *state)
{
	(void)state;
	return 0;
}

static const struct stage g_stage = {
	.name = "ring_output",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD),
	.out_type = PORT_TYPE_RAW_RECORD, /* unused for a leaf - see stage.h's
					      out_port_count comment */
	.init = ring_output_stage_init,
	.out_port_count = ring_output_out_port_count,
	.process = ring_output_stage_process,
	.teardown = ring_output_stage_teardown,
	.get_status = ring_output_stage_get_status,
	.get_config_schema = ring_output_stage_get_config_schema,
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
