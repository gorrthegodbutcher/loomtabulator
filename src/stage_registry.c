#include <string.h>
#include <stddef.h>
#include "stage_registry.h"
#include "stages/validate_stage.h"
#include "stages/extract_stage.h"
#include "stages/convert_stage.h"
#include "stages/forward_udp_stage.h"

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
		.process = forward_udp_stage_process,
		.teardown = forward_udp_stage_teardown,
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
