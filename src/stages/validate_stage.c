#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include "validate_stage.h"
#include "../record.h"

/* PORT_TYPE_RAW_RECORD -> PORT_TYPE_RAW_RECORD (same shape either way -
 * see stage.h's enum comment for why "checked or not" isn't its own
 * type). Confirms a record read off the input ring is actually
 * well-formed before anything downstream trusts it - the same "check
 * once, trust after" shape as chrontabulator's own magic==0 padding
 * check, just at the front of this pipeline instead of at read time off
 * disk.
 *
 * Two kinds of failure, handled differently:
 *   - too short, or hdr->len inconsistent with the record's actual
 *     size: genuinely unparseable - nothing downstream could safely
 *     compute payload bounds from a length field that doesn't match
 *     reality, so this is always a hard ok=false drop, not configurable.
 *   - bad magic: the record's SHAPE is still trustworthy (length
 *     accounting checks out), it just doesn't look like a genuine
 *     capture record - a content judgment, not a structural one, so
 *     it's flagged (STAGE_RECORD_FLAG_INTEGRITY_FAILED) and passed
 *     through rather than hard-dropped. What actually happens to a
 *     flagged record (dropped anyway, passed on untouched, or routed to
 *     a dedicated diagnostic edge) is graph_config.c's/pipeline.c's
 *     call, not this stage's - see stage.h's flag comment. */

struct validate_config {
	bool require_magic;

	/* Exposed via get_status() below - see stage.h's own comment on why
	 * these need to be atomics: process() can be called concurrently by
	 * every worker lcore that routes a record to THIS instance, while
	 * get_status() reads them from the main lcore on a completely
	 * separate, much slower cadence. "checked" only counts records that
	 * got far enough to actually have their magic examined (the two
	 * structural hard-drops below never reach this point) - "flagged"
	 * is the subset of those with a bad magic. */
	atomic_uint_least64_t checked;
	atomic_uint_least64_t flagged;
};

void *
validate_stage_init(const struct json_value *config)
{
	struct validate_config *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;
	/* Default true - a record without a sane magic is almost certainly
	 * ring corruption or a producer bug, not something a real pipeline
	 * should ever want to silently downgrade to "off" without a reason;
	 * the config knob exists mainly for test fixtures that want to
	 * exercise downstream stages without building a real header. */
	st->require_magic = json_as_bool(json_object_get(config, "require_magic"), true);
	atomic_init(&st->checked, 0);
	atomic_init(&st->flagged, 0);
	return st;
}

struct stage_result
validate_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct validate_config *cfg = state;

	if (in->len < sizeof(struct chrono_record_hdr))
		return (struct stage_result){ .ok = false, .drop_reason = "shorter than chrono_record_hdr" };

	const struct chrono_record_hdr *hdr = (const struct chrono_record_hdr *)in->data;

	if ((uint64_t)sizeof(*hdr) + hdr->len != in->len)
		return (struct stage_result){ .ok = false, .drop_reason = "hdr->len doesn't match record size" };

	bool magic_bad = cfg->require_magic && hdr->magic != CHRONO_RECORD_MAGIC;
	atomic_fetch_add_explicit(&cfg->checked, 1, memory_order_relaxed);
	if (magic_bad)
		atomic_fetch_add_explicit(&cfg->flagged, 1, memory_order_relaxed);

	/* Always a full copy (not aliasing in->data), same "every stage owns
	 * its own scratch buffer" simplicity as every other v1 stage - see
	 * pipeline.c's own comment on why zero-copy is a later optimization,
	 * not a v1 concern. */
	memcpy(out->data, in->data, in->len);
	out->type = PORT_TYPE_RAW_RECORD;
	out->len = in->len;
	out->capture_tsc = in->capture_tsc;
	if (magic_bad)
		out->flags |= STAGE_RECORD_FLAG_INTEGRITY_FAILED;
	return (struct stage_result){ .ok = true };
}

void
validate_stage_teardown(void *state)
{
	free(state);
}

void
validate_stage_get_status(void *state, struct stage_status *out)
{
	struct validate_config *cfg = state;
	out->field_count = 2;
	snprintf(out->fields[0].name, STAGE_STATUS_NAME_MAX, "records_checked");
	out->fields[0].value = atomic_load_explicit(&cfg->checked, memory_order_relaxed);
	snprintf(out->fields[1].name, STAGE_STATUS_NAME_MAX, "records_flagged");
	out->fields[1].value = atomic_load_explicit(&cfg->flagged, memory_order_relaxed);
}
