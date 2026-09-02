/* A minimal, fully valid loomlet fixture for tests/test_plugin_loader.c -
 * exercises loom_stage_entry_at()'s multi-stage path, including that
 * both stages share one dlopen() handle (see plugin_loader.c's own
 * register_stage()/plugin_loader_shutdown() comments for why that
 * matters). Deliberately does NOT export loom_stage_entry() at all -
 * proving a loomlet doesn't need to keep the single-stage export
 * around too. */
#include "../../src/stage_abi.h"

static struct stage_result
fixture_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	(void)state; (void)in; (void)out;
	return (struct stage_result){ .ok = true };
}

static const struct stage g_stage_a = {
	.name = "fixture_loomlet_a",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD),
	.out_type = PORT_TYPE_RAW_RECORD,
	.process = fixture_process,
};

static const struct stage g_stage_b = {
	.name = "fixture_loomlet_b",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD),
	.out_type = PORT_TYPE_RAW_RECORD,
	.process = fixture_process,
};

uint32_t
loom_stage_abi_version(void)
{
	return STAGE_ABI_VERSION;
}

const struct stage *
loom_stage_entry_at(unsigned index)
{
	switch (index) {
	case 0: return &g_stage_a;
	case 1: return &g_stage_b;
	default: return NULL;
	}
}
