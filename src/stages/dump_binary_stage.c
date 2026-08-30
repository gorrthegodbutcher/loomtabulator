#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "dump_binary_stage.h"

/* raw_record/wire_frame -> nothing (a leaf, see its plugin shim's
 * out_port_count). Writes every record's bytes verbatim to a file, in
 * order, no framing/length-prefixing added - the generic "capture
 * whatever's flowing at this point in the graph" debugging tool, since
 * both of its accepted types are already opaque byte blobs with no
 * semantic value needing interpretation (unlike PORT_TYPE_ENGINEERING -
 * see forward_udp_stage.c's header comment for why that distinction
 * matters for a stage like this). */

struct dump_binary_config {
	FILE *f;

	/* Exposed via get_status() below - see stage.h's own comment on why
	 * these need to be atomics (concurrent worker-lcore writers, a
	 * main-lcore reader on a separate, much slower cadence). */
	atomic_uint_least64_t records_written;
	atomic_uint_least64_t bytes_written;
};

void *
dump_binary_stage_init(const struct json_value *config)
{
	const char *path = json_as_string(json_object_get(config, "path"), NULL);
	if (path == NULL)
		return NULL;

	struct dump_binary_config *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;

	st->f = fopen(path, "wb");
	if (st->f == NULL) {
		free(st);
		return NULL;
	}
	atomic_init(&st->records_written, 0);
	atomic_init(&st->bytes_written, 0);
	return st;
}

struct stage_result
dump_binary_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct dump_binary_config *cfg = state;
	(void)out; /* a leaf - nothing downstream ever reads *out */

	if (in->len > 0 && fwrite(in->data, 1, in->len, cfg->f) != in->len)
		return (struct stage_result){ .ok = false, .drop_reason = "fwrite() failed" };

	atomic_fetch_add_explicit(&cfg->records_written, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&cfg->bytes_written, in->len, memory_order_relaxed);
	return (struct stage_result){ .ok = true };
}

void
dump_binary_stage_teardown(void *state)
{
	struct dump_binary_config *cfg = state;
	if (cfg != NULL)
		fclose(cfg->f);
	free(cfg);
}

void
dump_binary_stage_get_status(void *state, struct stage_status *out)
{
	struct dump_binary_config *cfg = state;
	out->field_count = 2;
	snprintf(out->fields[0].name, STAGE_STATUS_NAME_MAX, "records_written");
	out->fields[0].value = atomic_load_explicit(&cfg->records_written, memory_order_relaxed);
	snprintf(out->fields[1].name, STAGE_STATUS_NAME_MAX, "bytes_written");
	out->fields[1].value = atomic_load_explicit(&cfg->bytes_written, memory_order_relaxed);
}
