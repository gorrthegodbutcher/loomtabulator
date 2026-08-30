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

/* Test-only 2-port stage, exercising graph_config.c's dynamic
 * out_port_count() wiring/validation (tests/test_graph_config.c) -
 * routes by the first payload byte's parity, so it's a real, correct,
 * deterministic router rather than a dead stub, even though
 * test_graph_config.c itself only ever calls graph_config_load()
 * (init()/out_port_count()), never process(). */
static unsigned
multi_out_stub_port_count(void *state)
{
	(void)state;
	return 2;
}

static struct stage_result
multi_out_stub_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	(void)state;
	memcpy(out->data, in->data, in->len);
	out->len = in->len;
	out->type = PORT_TYPE_VALIDATED;
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true, .out_port = (in->len > 0 && in->data[0] % 2 == 1) ? 1u : 0u };
}

/* Mirrors src/stages/forward_udp_stage_plugin.c's real leaf declaration
 * exactly - this stub's forward_udp entry has to behave the same way
 * the real plugin does (0 output ports), or "loads the v1 example
 * graph" below would start requiring a wired outgoing edge on
 * example_graph.json's forward_udp node, which doesn't have one. Shared
 * by "forward_udp" and "leaf_stub" below - both are genuine leaves. */
static unsigned
zero_out_ports(void *state)
{
	(void)state;
	return 0;
}

/* Test-only leaf stage, in_type = VALIDATED (so it can sit directly
 * after multi_out_stub above without needing a full
 * extract->convert->forward_udp tail on each branch) - keeps
 * test_graph_config.c's branching-graph tests small and focused on the
 * wiring/validation logic itself, not on chaining real production
 * stage types together. */
static struct stage_result
leaf_stub_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	(void)state;
	memcpy(out->data, in->data, in->len);
	out->len = in->len;
	out->type = PORT_TYPE_WIRE_FRAME;
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true };
}

static const struct stage g_registry[] = {
	{
		.name = "validate",
		.in_type = PORT_TYPE_RAW_RECORD,
		.out_type = PORT_TYPE_VALIDATED,
		.init = validate_stage_init,
		.process = validate_stage_process,
		.teardown = validate_stage_teardown,
	},
	{
		.name = "extract",
		.in_type = PORT_TYPE_VALIDATED,
		.out_type = PORT_TYPE_EXTRACTED,
		.init = extract_stage_init,
		.process = extract_stage_process,
		.teardown = extract_stage_teardown,
	},
	{
		.name = "convert",
		.in_type = PORT_TYPE_EXTRACTED,
		.out_type = PORT_TYPE_ENGINEERING,
		.init = convert_stage_init,
		.process = convert_stage_process,
		.teardown = convert_stage_teardown,
	},
	{
		.name = "forward_udp",
		.in_type = PORT_TYPE_ENGINEERING,
		.out_type = PORT_TYPE_WIRE_FRAME,
		.init = forward_udp_stage_init,
		.out_port_count = zero_out_ports,
		.process = forward_udp_stage_process,
		.teardown = forward_udp_stage_teardown,
	},
	{
		.name = "multi_out_stub",
		.in_type = PORT_TYPE_RAW_RECORD,
		.out_type = PORT_TYPE_VALIDATED,
		.init = validate_stage_init,
		.out_port_count = multi_out_stub_port_count,
		.process = multi_out_stub_process,
		.teardown = validate_stage_teardown,
	},
	{
		.name = "leaf_stub",
		.in_type = PORT_TYPE_VALIDATED,
		.out_type = PORT_TYPE_WIRE_FRAME,
		.init = validate_stage_init,
		.out_port_count = zero_out_ports,
		.process = leaf_stub_process,
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
