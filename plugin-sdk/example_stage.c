/* Minimal worked example of a loomtabulator stage plugin - a
 * passthrough stage that copies its input record to its output
 * unchanged, optionally printing one line per record if its config
 * says to. Build standalone, no loomtabulator checkout required:
 *
 *   cc -O2 -Wall -Wextra -fPIC -I. -shared -o example.so example_stage.c json.c
 *
 * (run from inside plugin-sdk/, or adjust -I/paths accordingly). Drop
 * the resulting example.so into loomtabulator's --plugins-dir and it
 * shows up as stage type "example" in GET /api/stage-types. See this
 * directory's README.md for the full set of build rules.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stage_abi.h"

struct example_state {
	bool verbose;
};

static void *
example_init(const struct json_value *config)
{
	struct example_state *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;

	st->verbose = json_as_bool(json_object_get(config, "verbose"), false);
	return st;
}

static struct stage_result
example_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct example_state *st = state;

	if (st != NULL && st->verbose)
		fprintf(stderr, "example: passing through %u bytes\n", in->len);

	memcpy(out->data, in->data, in->len);
	out->len = in->len;
	out->type = in->type;
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true };
}

static void
example_teardown(void *state)
{
	free(state);
}

/* A passthrough stage doesn't transform the record's meaning, so its
 * declared in_type/out_type match - pick whichever port type this
 * stage actually sits between in your own graph; PORT_TYPE_EXTRACTED
 * is just this example's arbitrary choice. */
static const struct stage g_stage = {
	.name = "example",
	.in_type = PORT_TYPE_EXTRACTED,
	.out_type = PORT_TYPE_EXTRACTED,
	.max_out_ports = 1,
	.init = example_init,
	.process = example_process,
	.teardown = example_teardown,
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
