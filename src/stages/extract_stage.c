#include <stdlib.h>
#include "extract_stage.h"
#include "../record.h"

/* PORT_TYPE_VALIDATED -> PORT_TYPE_EXTRACTED. Pulls one fixed-width,
 * fixed-offset field out of the record's payload (big-endian on the
 * wire, same convention as common.c's own put_be16/put_be32/get_be16
 * helpers use everywhere else in this project family) and re-encodes it
 * as a canonical 8-byte big-endian unsigned value - regardless of the
 * field's original width - so convert_stage.c (and any future stage
 * type downstream of extract) only ever has one shape to read, not
 * three. Multiple extract instances in a chain can each pull a
 * different field if a future graph wants more than one value out of
 * the same record - v1's example graph only uses one. */

struct extract_config {
	uint32_t field_offset_bytes;
	uint32_t field_width_bytes; /* 2, 4, or 8 - validated at init() */
};

static uint64_t
get_be(const uint8_t *p, uint32_t width)
{
	uint64_t v = 0;
	for (uint32_t i = 0; i < width; i++)
		v = (v << 8) | p[i];
	return v;
}

static void
put_be64(uint8_t *p, uint64_t v)
{
	for (int i = 7; i >= 0; i--) {
		p[i] = (uint8_t)v;
		v >>= 8;
	}
}

void *
extract_stage_init(const struct json_value *config)
{
	uint32_t width = (uint32_t)json_as_number(json_object_get(config, "field_width_bytes"), 0);
	if (width != 2 && width != 4 && width != 8)
		return NULL; /* graph_config.c surfaces this as a startup error */

	struct extract_config *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;
	st->field_offset_bytes =
		(uint32_t)json_as_number(json_object_get(config, "field_offset_bytes"), 0);
	st->field_width_bytes = width;
	return st;
}

struct stage_result
extract_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct extract_config *cfg = state;
	const struct chrono_record_hdr *hdr = (const struct chrono_record_hdr *)in->data;
	const uint8_t *payload = in->data + sizeof(*hdr);

	if ((uint64_t)cfg->field_offset_bytes + cfg->field_width_bytes > hdr->len)
		return (struct stage_result){ .ok = false,
			.drop_reason = "extract field falls outside payload" };

	uint64_t raw = get_be(payload + cfg->field_offset_bytes, cfg->field_width_bytes);
	put_be64(out->data, raw);
	out->type = PORT_TYPE_EXTRACTED;
	out->len = 8;
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true };
}

void
extract_stage_teardown(void *state)
{
	free(state);
}
