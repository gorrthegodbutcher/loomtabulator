#include <string.h>
#include <stdlib.h>
#include "convert_stage.h"

/* PORT_TYPE_EXTRACTED -> PORT_TYPE_ENGINEERING. The classic DAQ/telemetry
 * "raw counts to engineering units" linear calibration:
 * engineering = raw * scale + offset. Anything more elaborate
 * (piecewise, polynomial, lookup-table calibration curves) is a
 * different future stage type, not a config option bolted onto this
 * one - keeps this stage's config schema (and its web-UI-editable form,
 * once Phase 3 exists) simple and obvious. */

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
	 * deployment target) - this is an internal pipeline value, not yet
	 * on the wire. forward_udp_stage.c is what re-encodes it as bytes
	 * for actual transmission, and does its own big-endian conversion
	 * there since that value genuinely does go on the wire. */
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
