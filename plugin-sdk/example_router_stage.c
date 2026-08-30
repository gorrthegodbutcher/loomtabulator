/* Second worked example - a multi-output router, demonstrating
 * struct stage.out_port_count() and struct stage_result.out_port (see
 * stage.h and this directory's README.md "Output ports" section).
 * Reads a config-driven routing table and picks one of several output
 * ports per record based on the record's last payload byte (the
 * low-order byte of a big-endian multi-byte value, e.g. a counter -
 * the byte actually likely to vary from record to record; see
 * README.md's "Verifying it end-to-end" for why this matters when
 * this stage sits upstream of `convert`, which always expects exactly
 * an 8-byte big-endian value) -
 * deliberately simpler than a real field-name/CID abstraction (see
 * docs/RECOMMENDATIONS.md's proposal for the motivating use case); this
 * is a template to copy and adapt, not a reusable routing framework.
 *
 * Config shape:
 *   {
 *     "routes": [ { "byte_value": 3, "port": 0 },
 *                 { "byte_value": 7, "port": 1 } ],
 *     "default_port": 2
 *   }
 * A record whose last byte matches a "byte_value" entry routes to
 * that entry's "port"; anything else routes to "default_port" (0 if
 * omitted). out_port_count() returns one more than the highest port
 * number actually used, so a graph wiring this stage gets an exact
 * "declared N ports, must wire exactly N edges" check from
 * graph_config.c - see stage.h's out_port_count comment.
 *
 * Build standalone, no loomtabulator checkout required:
 *
 *   cc -O2 -Wall -Wextra -fPIC -I. -shared -o example_router.so example_router_stage.c json.c
 *
 * (run from inside plugin-sdk/, or adjust -I/paths accordingly).
 */
#include <stdlib.h>
#include <string.h>
#include "stage_abi.h"

struct route_entry {
	uint8_t byte_value;
	unsigned port;
};

struct router_state {
	struct route_entry *routes; /* NULL if route_count == 0 */
	size_t route_count;
	unsigned default_port;
	unsigned port_count;
};

static void *
router_init(const struct json_value *config)
{
	const struct json_value *routes_json = json_object_get(config, "routes");
	size_t route_count = json_array_size(routes_json);

	struct router_state *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;

	/* calloc(0, ...) is legal but implementation-defined whether it
	 * returns NULL - only treat a NULL return as allocation failure
	 * when a nonzero count was actually requested, or "no routes
	 * configured, always use default_port" would wrongly fail init(). */
	struct route_entry *routes = NULL;
	if (route_count > 0) {
		routes = calloc(route_count, sizeof(*routes));
		if (routes == NULL) {
			free(st);
			return NULL;
		}
	}
	st->routes = routes;
	st->route_count = route_count;

	unsigned max_port = 0;
	for (size_t i = 0; i < route_count; i++) {
		const struct json_value *r = json_array_get(routes_json, i);
		st->routes[i].byte_value = (uint8_t)json_as_number(json_object_get(r, "byte_value"), 0);
		st->routes[i].port = (unsigned)json_as_number(json_object_get(r, "port"), 0);
		if (st->routes[i].port > max_port)
			max_port = st->routes[i].port;
	}
	st->default_port = (unsigned)json_as_number(json_object_get(config, "default_port"), 0);
	if (st->default_port > max_port)
		max_port = st->default_port;
	st->port_count = max_port + 1;

	return st;
}

static unsigned
router_out_port_count(void *state)
{
	struct router_state *st = state;
	return st->port_count;
}

static struct stage_result
router_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct router_state *st = state;
	unsigned port = st->default_port;

	if (in->len > 0) {
		uint8_t b = in->data[in->len - 1];
		for (size_t i = 0; i < st->route_count; i++) {
			if (st->routes[i].byte_value == b) {
				port = st->routes[i].port;
				break;
			}
		}
	}

	memcpy(out->data, in->data, in->len);
	out->len = in->len;
	out->type = in->type;
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true, .out_port = port };
}

static void
router_teardown(void *state)
{
	struct router_state *st = state;
	if (st != NULL)
		free(st->routes);
	free(st);
}

/* A router doesn't transform the record's meaning, so in_type/out_type
 * match, same as example_stage.c's passthrough - pick whichever port
 * type this stage actually sits between in your own graph. */
static const struct stage g_stage = {
	.name = "example_router",
	.in_type = PORT_TYPE_EXTRACTED,
	.out_type = PORT_TYPE_EXTRACTED,
	.init = router_init,
	.out_port_count = router_out_port_count,
	.process = router_process,
	.teardown = router_teardown,
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
