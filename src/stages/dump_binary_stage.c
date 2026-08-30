#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	return st;
}

struct stage_result
dump_binary_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct dump_binary_config *cfg = state;
	(void)out; /* a leaf - nothing downstream ever reads *out */

	if (in->len > 0 && fwrite(in->data, 1, in->len, cfg->f) != in->len)
		return (struct stage_result){ .ok = false, .drop_reason = "fwrite() failed" };

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
