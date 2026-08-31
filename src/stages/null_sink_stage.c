#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include "null_sink_stage.h"

/* raw_record/engineering/wire_frame -> nothing (a leaf, see its plugin
 * shim's out_port_count). The data-sink stage: accepts literally
 * anything (every port type that exists - see its plugin shim's
 * in_types), counts records and bytes received, and discards the data
 * silently - no file, no socket, no side effect beyond the counters
 * get_status() below reports. Useful as a graph terminator when you
 * want to measure what's flowing through a branch (or drop what a
 * validate/extract stage flags as invalid - see its "invalid_target"
 * edge) without actually persisting or transmitting it anywhere.
 *
 * No config - there is nothing for this stage to be configured with. */

struct null_sink_config {
	/* Exposed via get_status() below - see stage.h's own comment on why
	 * these need to be atomics (concurrent worker-lcore writers, a
	 * main-lcore reader on a separate, much slower cadence). */
	atomic_uint_least64_t records_received;
	atomic_uint_least64_t bytes_received;
};

void *
null_sink_stage_init(const struct json_value *config)
{
	(void)config; /* nothing to read - this stage takes no config at all */

	struct null_sink_config *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;
	atomic_init(&st->records_received, 0);
	atomic_init(&st->bytes_received, 0);
	return st;
}

struct stage_result
null_sink_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct null_sink_config *cfg = state;
	(void)out; /* a leaf - nothing downstream ever reads *out */

	atomic_fetch_add_explicit(&cfg->records_received, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&cfg->bytes_received, in->len, memory_order_relaxed);
	return (struct stage_result){ .ok = true };
}

void
null_sink_stage_teardown(void *state)
{
	free(state);
}

void
null_sink_stage_get_status(void *state, struct stage_status *out)
{
	struct null_sink_config *cfg = state;
	out->field_count = 2;
	snprintf(out->fields[0].name, STAGE_STATUS_NAME_MAX, "records_received");
	out->fields[0].value = atomic_load_explicit(&cfg->records_received, memory_order_relaxed);
	snprintf(out->fields[1].name, STAGE_STATUS_NAME_MAX, "bytes_received");
	out->fields[1].value = atomic_load_explicit(&cfg->bytes_received, memory_order_relaxed);
}
