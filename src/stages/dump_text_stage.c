#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dump_text_stage.h"

/* engineering -> nothing (a leaf, see its plugin shim's
 * out_port_count) - the built-in consumer of convert_stage.c's output
 * now that forward_udp no longer accepts PORT_TYPE_ENGINEERING
 * directly (see forward_udp_stage.c's own header comment). Formats
 * each record's double as ASCII text, one value per write, with a
 * carriage return (0x0D) appended - not a newline (0x0A). That's a
 * literal reading of the original request ("carriage returns appended
 * to the values"); most Unix tools expect '\n' to start a new line, so
 * `cat`-ing the resulting file will show every value overwriting the
 * same terminal line rather than one value per line. Switch the '\r'
 * below to '\n' (or "\r\n") if that's not actually what's wanted. */

struct dump_text_config {
	FILE *f;
};

void *
dump_text_stage_init(const struct json_value *config)
{
	const char *path = json_as_string(json_object_get(config, "path"), NULL);
	if (path == NULL)
		return NULL;

	struct dump_text_config *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;

	st->f = fopen(path, "w");
	if (st->f == NULL) {
		free(st);
		return NULL;
	}
	return st;
}

struct stage_result
dump_text_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct dump_text_config *cfg = state;
	(void)out; /* a leaf - nothing downstream ever reads *out */

	if (in->len != 8)
		return (struct stage_result){ .ok = false, .drop_reason = "expected 8-byte engineering value" };

	double value;
	memcpy(&value, in->data, sizeof(value));

	if (fprintf(cfg->f, "%.17g\r", value) < 0)
		return (struct stage_result){ .ok = false, .drop_reason = "fprintf() failed" };

	return (struct stage_result){ .ok = true };
}

void
dump_text_stage_teardown(void *state)
{
	struct dump_text_config *cfg = state;
	if (cfg != NULL)
		fclose(cfg->f);
	free(cfg);
}
