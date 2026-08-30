/* Frozen copy of the pre-plugin-conversion static stage registry -
 * test-only scaffolding for test_graph_config.c, which only needs
 * graph_config_load()'s chain-building/validation logic exercised
 * against SOME correctly-populated registry, not the real dlopen()
 * machinery (that's tests/test_plugin_loader.c's job). Keeping this
 * frozen and separate means test_graph_config.c stays a plain,
 * fast, dependency-free host binary exactly as it always has been. */
#include <string.h>
#include <stddef.h>
#include "../src/plugin_loader.h"
#include "../src/stages/validate_stage.h"
#include "../src/stages/extract_stage.h"
#include "../src/stages/convert_stage.h"
#include "../src/stages/forward_udp_stage.h"

static const struct stage g_registry[] = {
	{
		.name = "validate",
		.in_type = PORT_TYPE_RAW_RECORD,
		.out_type = PORT_TYPE_VALIDATED,
		.max_out_ports = 1,
		.init = validate_stage_init,
		.process = validate_stage_process,
		.teardown = validate_stage_teardown,
	},
	{
		.name = "extract",
		.in_type = PORT_TYPE_VALIDATED,
		.out_type = PORT_TYPE_EXTRACTED,
		.max_out_ports = 1,
		.init = extract_stage_init,
		.process = extract_stage_process,
		.teardown = extract_stage_teardown,
	},
	{
		.name = "convert",
		.in_type = PORT_TYPE_EXTRACTED,
		.out_type = PORT_TYPE_ENGINEERING,
		.max_out_ports = 1,
		.init = convert_stage_init,
		.process = convert_stage_process,
		.teardown = convert_stage_teardown,
	},
	{
		.name = "forward_udp",
		.in_type = PORT_TYPE_ENGINEERING,
		.out_type = PORT_TYPE_WIRE_FRAME,
		.max_out_ports = 1,
		.init = forward_udp_stage_init,
		.process = forward_udp_stage_process,
		.teardown = forward_udp_stage_teardown,
	},
	/* Test-only: a stage type declaring more than one output port, to
	 * exercise graph_config.c's max_out_ports guard clause
	 * (tests/test_graph_config.c). Never actually reached - the guard
	 * fires before init/process/teardown would ever be called - so
	 * reusing validate_stage's functions here is arbitrary, not a
	 * meaningful choice. */
	{
		.name = "multi_out_stub",
		.in_type = PORT_TYPE_RAW_RECORD,
		.out_type = PORT_TYPE_VALIDATED,
		.max_out_ports = 2,
		.init = validate_stage_init,
		.process = validate_stage_process,
		.teardown = validate_stage_teardown,
	},
};

#define REGISTRY_COUNT (sizeof(g_registry) / sizeof(g_registry[0]))

const struct stage *
stage_registry_find(const char *name)
{
	for (size_t i = 0; i < REGISTRY_COUNT; i++)
		if (strcmp(g_registry[i].name, name) == 0)
			return &g_registry[i];
	return NULL;
}

size_t
stage_registry_count(void)
{
	return REGISTRY_COUNT;
}

const struct stage *
stage_registry_get(size_t idx)
{
	return idx < REGISTRY_COUNT ? &g_registry[idx] : NULL;
}

const char *
stage_port_type_name(enum stage_port_type type)
{
	switch (type) {
	case PORT_TYPE_RAW_RECORD:  return "raw_record";
	case PORT_TYPE_VALIDATED:   return "validated";
	case PORT_TYPE_EXTRACTED:   return "extracted";
	case PORT_TYPE_ENGINEERING: return "engineering";
	case PORT_TYPE_WIRE_FRAME:  return "wire_frame";
	default:                    return "unknown";
	}
}
