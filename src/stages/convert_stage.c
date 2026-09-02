#include <string.h>
#include <stdlib.h>
#include "convert_stage.h"

/* PORT_TYPE_RAW_RECORD -> PORT_TYPE_ENGINEERING. The classic DAQ/telemetry
 * "raw counts to engineering units" linear calibration:
 * engineering = raw * scale + offset. Anything more elaborate
 * (piecewise, polynomial, lookup-table calibration curves) is a
 * different future stage type, not a config option bolted onto this
 * one - keeps this stage's config schema (and its web-UI-editable form,
 * once Phase 3 exists) simple and obvious.
 *
 * The type system no longer distinguishes "raw bytes" from "extract's
 * numeric-mode output" (both are just PORT_TYPE_RAW_RECORD - see
 * stage.h's enum comment) - this stage's own runtime len==8 check below
 * is what actually enforces "did extract's numeric mode really run
 * first," not graph-time type validation. */

struct convert_config {
	double scale;
	double offset;
};

static uint64_t
get_be64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

void *
convert_stage_init(const struct json_value *config)
{
	struct convert_config *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;
	st->scale = json_as_number(json_object_get(config, "scale"), 1.0);
	st->offset = json_as_number(json_object_get(config, "offset"), 0.0);
	return st;
}

struct stage_result
convert_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct convert_config *cfg = state;

	if (in->len != 8)
		return (struct stage_result){ .ok = false, .drop_reason = "expected 8-byte extracted value" };

	uint64_t raw = get_be64(in->data);
	double engineering = (double)raw * cfg->scale + cfg->offset;

	/* Host byte order (x86_64, always little-endian for this project's
	 * deployment target) - this is an internal pipeline value, an
	 * opaque double bit pattern, not yet encoded for any particular
	 * destination. forward_udp doesn't accept PORT_TYPE_ENGINEERING at
	 * all (a double isn't just an opaque byte blob the way
	 * raw_record is - see forward_udp_stage.c's own header comment);
	 * dump_text_stage.c is the built-in consumer that knows how to turn
	 * this into a specific on-disk encoding (ASCII text). */
	memcpy(out->data, &engineering, sizeof(engineering));
	out->type = PORT_TYPE_ENGINEERING;
	out->len = sizeof(engineering);
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true };
}

void
convert_stage_teardown(void *state)
{
	free(state);
}

void
convert_stage_get_config_schema(struct stage_config_schema *out)
{
	out->field_count = 2;
	out->fields[0] = (struct stage_config_field){
		.name = "scale",
		.type = CONFIG_FIELD_NUMBER,
		.description = "engineering = raw * scale + offset",
		.has_default = true,
		.default_value = "1.0",
	};
	out->fields[1] = (struct stage_config_field){
		.name = "offset",
		.type = CONFIG_FIELD_NUMBER,
		.description = "engineering = raw * scale + offset",
		.has_default = true,
		.default_value = "0.0",
	};
}
