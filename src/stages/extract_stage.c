#include <stdlib.h>
#include <string.h>
#include "extract_stage.h"
#include "../record.h"

/* PORT_TYPE_RAW_RECORD -> PORT_TYPE_RAW_RECORD (same shape either way -
 * see stage.h's enum comment; "extracted" isn't its own type anymore).
 * Two mutually exclusive modes, picked by the config's "mode" string:
 *
 * - "numeric" (default, the original v1 behavior): pulls one
 *   fixed-width, fixed-offset field out of the record's payload
 *   (big-endian on the wire, same convention as common.c's own
 *   put_be16/put_be32/get_be16 helpers use everywhere else in this
 *   project family) and re-encodes it as a canonical 8-byte big-endian
 *   unsigned value - regardless of the field's original width - so
 *   convert_stage.c only ever has one shape to read, not three.
 *   convert_stage_process() requires exactly this 8-byte shape, so
 *   this is the only mode that can feed convert - the type system
 *   doesn't enforce that anymore (both modes emit PORT_TYPE_RAW_RECORD),
 *   convert's own runtime length check is what actually catches the
 *   mistake of wiring bytes-mode output (or anything else) into convert.
 * - "bytes": copies a byte range out of the payload verbatim, any
 *   offset/length, with no reinterpretation at all - for a downstream
 *   stage (a third-party plugin, dump_binary, forward_udp) that wants
 *   a raw sub-slice of the payload rather than a single re-encoded
 *   number. Output length is whatever the config asked for, not always
 *   8 bytes - since the output is just PORT_TYPE_RAW_RECORD like any
 *   other opaque byte blob, this can feed straight into any stage that
 *   already accepts raw_record (forward_udp, dump_binary, a third-party
 *   plugin), with zero type-system friction.
 *
 * Multiple extract instances in a chain can each pull a different
 * field/slice if a future graph wants more than one value out of the
 * same record - v1's example graph only uses one.
 *
 * Failure (the configured offset/width or offset/length doesn't fit
 * inside the record's actual payload) is flagged
 * (STAGE_RECORD_FLAG_INTEGRITY_FAILED) and passed through as the whole
 * original record, not hard-dropped - see extract_stage_process()'s own
 * comment for why. */

enum extract_mode {
	EXTRACT_MODE_NUMERIC,
	EXTRACT_MODE_BYTES,
};

struct extract_config {
	enum extract_mode mode;
	uint32_t field_offset_bytes;
	uint32_t field_width_bytes;  /* numeric mode: 2, 4, or 8 - validated at init() */
	uint32_t field_length_bytes; /* bytes mode: 1..STAGE_SCRATCH_BYTES - validated at init() */
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
	const char *mode_str = json_as_string(json_object_get(config, "mode"), "numeric");

	struct extract_config *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;
	st->field_offset_bytes =
		(uint32_t)json_as_number(json_object_get(config, "field_offset_bytes"), 0);

	if (strcmp(mode_str, "numeric") == 0) {
		uint32_t width = (uint32_t)json_as_number(json_object_get(config, "field_width_bytes"), 0);
		if (width != 2 && width != 4 && width != 8) {
			free(st); /* graph_config.c surfaces this as a startup error */
			return NULL;
		}
		st->mode = EXTRACT_MODE_NUMERIC;
		st->field_width_bytes = width;
	} else if (strcmp(mode_str, "bytes") == 0) {
		uint32_t length = (uint32_t)json_as_number(json_object_get(config, "field_length_bytes"), 0);
		if (length < 1 || length > STAGE_SCRATCH_BYTES) {
			free(st);
			return NULL;
		}
		st->mode = EXTRACT_MODE_BYTES;
		st->field_length_bytes = length;
	} else {
		free(st); /* unknown mode - startup error, not a silent fallback */
		return NULL;
	}

	return st;
}

struct stage_result
extract_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct extract_config *cfg = state;
	const struct chrono_record_hdr *hdr = (const struct chrono_record_hdr *)in->data;
	const uint8_t *payload = in->data + sizeof(*hdr);

	bool out_of_range = cfg->mode == EXTRACT_MODE_NUMERIC
		? (uint64_t)cfg->field_offset_bytes + cfg->field_width_bytes > hdr->len
		: (uint64_t)cfg->field_offset_bytes + cfg->field_length_bytes > hdr->len;

	if (out_of_range) {
		/* Can't produce the requested field/slice - there's nothing to
		 * put in *out that would mean what the config asked for. But
		 * this is a config/data-shape mismatch, not a structurally
		 * unparseable record (hdr->len itself is fine), so - unlike
		 * validate.c's own two structural failure cases - it's flagged
		 * and passed through whole rather than hard-dropped: the
		 * caller can wire this stage's "on_invalid"/invalid-record
		 * edge to a dump_binary sink and get the exact original record
		 * bytes that failed, for offline diagnosis, instead of just a
		 * log line and a silently-uncounted drop. See stage.h's flag
		 * comment and pipeline.c's dispatch loop for the mechanism. */
		memcpy(out->data, in->data, in->len);
		out->type = PORT_TYPE_RAW_RECORD;
		out->len = in->len;
		out->capture_tsc = in->capture_tsc;
		out->flags |= STAGE_RECORD_FLAG_INTEGRITY_FAILED;
		return (struct stage_result){ .ok = true };
	}

	if (cfg->mode == EXTRACT_MODE_NUMERIC) {
		uint64_t raw = get_be(payload + cfg->field_offset_bytes, cfg->field_width_bytes);
		put_be64(out->data, raw);
		out->len = 8;
	} else {
		memcpy(out->data, payload + cfg->field_offset_bytes, cfg->field_length_bytes);
		out->len = cfg->field_length_bytes;
	}

	out->type = PORT_TYPE_RAW_RECORD;
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true };
}

void
extract_stage_teardown(void *state)
{
	free(state);
}
