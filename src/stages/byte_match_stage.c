#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "byte_match_stage.h"

/* PORT_TYPE_RAW_RECORD -> PORT_TYPE_RAW_RECORD (pass-through, like
 * validate - see stage.h's enum comment). Config-defined magic-byte
 * match at a config-defined offset - a generalized, configurable
 * version of validate_stage.c's own hardcoded chrono_record_hdr.magic
 * check, for matching arbitrary protocol signatures anywhere in the
 * record rather than just the fixed chrono header field.
 *
 * "magic" and (optional) "mask" are hex strings, e.g. "magic":
 * "DEADBEEF04", "mask": "FFFFFFFF04" - decoded once at init() into
 * fixed byte arrays, matched byte-by-byte as
 * (data[offset+i] & mask[i]) == (magic[i] & mask[i]). mask defaults to
 * all-0xFF (an exact match) if omitted - most callers just want a plain
 * byte comparison and shouldn't have to spell out a same-length mask of
 * all Fs to get one.
 *
 * Two distinct failure modes, matching validate_stage.c's own two-tier
 * split:
 *   - offset + magic length exceeding the record's actual length is
 *     genuinely unparseable (nothing to compare against) - always a
 *     hard ok=false drop, not configurable, same as validate's own
 *     length-accounting check.
 *   - a byte mismatch is NOT an error - the record's shape is still
 *     fine, it just doesn't match the configured signature. Flagged
 *     (STAGE_RECORD_FLAG_INTEGRITY_FAILED) and passed through
 *     unchanged rather than dropped: what actually happens next (drop
 *     anyway, pass through, or route to a dedicated next stage) is
 *     graph_config.c's/pipeline.c's call via that node's "on_invalid"
 *     and optional invalid-record edge - this stage needs no branching
 *     logic of its own, same mechanism validate/extract already lean
 *     on for their own non-fatal failure case. */

#define BYTE_MATCH_MAX_LEN 64

struct byte_match_config {
	uint32_t offset;
	uint32_t len;
	uint8_t magic[BYTE_MATCH_MAX_LEN];
	uint8_t mask[BYTE_MATCH_MAX_LEN];

	/* Exposed via get_status() below - see stage.h's own comment on why
	 * these need to be atomics (concurrent worker-lcore writers, a
	 * main-lcore reader on a separate, much slower cadence). */
	atomic_uint_least64_t checked;
	atomic_uint_least64_t matched;
	atomic_uint_least64_t unmatched;
};

static int
hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Decodes an even-length hex string ("DEADBEEF04") into out (bounded by
 * out_cap bytes). Returns the decoded byte count, or -1 on any parse
 * error (empty, odd length, a non-hex character, or too long for
 * out_cap) - just enough for this stage's own two config fields, not a
 * general-purpose hex parser. */
static int
hex_decode(const char *hex, uint8_t *out, size_t out_cap)
{
	size_t len = strlen(hex);
	if (len == 0 || len % 2 != 0)
		return -1;
	size_t n = len / 2;
	if (n > out_cap)
		return -1;
	for (size_t i = 0; i < n; i++) {
		int hi = hex_nibble(hex[2 * i]);
		int lo = hex_nibble(hex[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return (int)n;
}

void *
byte_match_stage_init(const struct json_value *config)
{
	const char *magic_hex = json_as_string(json_object_get(config, "magic"), NULL);
	if (magic_hex == NULL)
		return NULL;

	struct byte_match_config *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;

	int magic_len = hex_decode(magic_hex, st->magic, sizeof(st->magic));
	if (magic_len <= 0) {
		free(st);
		return NULL;
	}

	const char *mask_hex = json_as_string(json_object_get(config, "mask"), NULL);
	if (mask_hex != NULL) {
		int mask_len = hex_decode(mask_hex, st->mask, sizeof(st->mask));
		if (mask_len != magic_len) {
			/* Mismatched lengths - a real config error (which
			 * byte does an extra mask byte even apply to?), not
			 * something to silently truncate or pad. */
			free(st);
			return NULL;
		}
	} else {
		memset(st->mask, 0xFF, (size_t)magic_len);
	}

	st->len = (uint32_t)magic_len;
	st->offset = (uint32_t)json_as_number(json_object_get(config, "offset"), 0);

	atomic_init(&st->checked, 0);
	atomic_init(&st->matched, 0);
	atomic_init(&st->unmatched, 0);
	return st;
}

struct stage_result
byte_match_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct byte_match_config *cfg = state;

	uint64_t end = (uint64_t)cfg->offset + cfg->len;
	if (end > in->len)
		return (struct stage_result){
			.ok = false,
			.drop_reason = "offset + magic length exceeds record length",
		};

	bool match = true;
	const uint8_t *p = in->data + cfg->offset;
	for (uint32_t i = 0; i < cfg->len; i++) {
		if ((p[i] & cfg->mask[i]) != (uint8_t)(cfg->magic[i] & cfg->mask[i])) {
			match = false;
			break;
		}
	}

	atomic_fetch_add_explicit(&cfg->checked, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(match ? &cfg->matched : &cfg->unmatched, 1, memory_order_relaxed);

	/* Always a full copy (not aliasing in->data), same "every stage owns
	 * its own scratch buffer" simplicity as validate_stage.c's own
	 * pass-through. */
	memcpy(out->data, in->data, in->len);
	out->type = PORT_TYPE_RAW_RECORD;
	out->len = in->len;
	out->capture_tsc = in->capture_tsc;
	if (!match)
		out->flags |= STAGE_RECORD_FLAG_INTEGRITY_FAILED;
	return (struct stage_result){ .ok = true };
}

void
byte_match_stage_teardown(void *state)
{
	free(state);
}

void
byte_match_stage_get_status(void *state, struct stage_status *out)
{
	struct byte_match_config *cfg = state;
	out->field_count = 3;
	snprintf(out->fields[0].name, STAGE_STATUS_NAME_MAX, "records_checked");
	out->fields[0].value = atomic_load_explicit(&cfg->checked, memory_order_relaxed);
	snprintf(out->fields[1].name, STAGE_STATUS_NAME_MAX, "records_matched");
	out->fields[1].value = atomic_load_explicit(&cfg->matched, memory_order_relaxed);
	snprintf(out->fields[2].name, STAGE_STATUS_NAME_MAX, "records_unmatched");
	out->fields[2].value = atomic_load_explicit(&cfg->unmatched, memory_order_relaxed);
}
