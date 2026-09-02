/* Plugin ABI shim for the built-in "dump_text" stage - see
 * validate_stage_plugin.c's own header comment for the general shape
 * every one of these shims follows. */
#include "dump_text_stage.h"
#include "../stage_abi.h"

/* A genuine leaf, same reasoning as forward_udp_stage_plugin.c's own
 * out_port_count - it writes to a file and has nothing further to
 * route to. */
static unsigned
dump_text_out_port_count(void *state)
{
	(void)state;
	return 0;
}

static const struct stage g_stage = {
	.name = "dump_text",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_ENGINEERING),
	.out_type = PORT_TYPE_ENGINEERING, /* unused for a leaf - see stage.h's
					       out_port_count comment */
	.init = dump_text_stage_init,
	.out_port_count = dump_text_out_port_count,
	.process = dump_text_stage_process,
	.teardown = dump_text_stage_teardown,
	.get_config_schema = dump_text_stage_get_config_schema,
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
