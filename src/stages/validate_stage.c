#include <string.h>
#include <stdlib.h>
#include "validate_stage.h"
#include "../record.h"

/* PORT_TYPE_RAW_RECORD -> PORT_TYPE_VALIDATED. Confirms a record read off
 * the input ring is actually well-formed before anything downstream
 * trusts it - the same "check once, trust after" shape as chrontabulator's
 * own magic==0 padding check, just at the front of this pipeline instead
 * of at read time off disk. */

struct validate_config {
	bool require_magic;
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
	return st;
}

struct stage_result
validate_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct validate_config *cfg = state;

	if (in->len < sizeof(struct chrono_record_hdr))
		return (struct stage_result){ .ok = false, .drop_reason = "shorter than chrono_record_hdr" };

	const struct chrono_record_hdr *hdr = (const struct chrono_record_hdr *)in->data;

	if (cfg->require_magic && hdr->magic != CHRONO_RECORD_MAGIC)
		return (struct stage_result){ .ok = false, .drop_reason = "bad magic" };

	if ((uint64_t)sizeof(*hdr) + hdr->len != in->len)
		return (struct stage_result){ .ok = false, .drop_reason = "hdr->len doesn't match record size" };

	/* Same wire shape as RAW_RECORD - just relabeled once it's known
	 * sane. Always a full copy (not aliasing in->data), same "every
	 * stage owns its own scratch buffer" simplicity as every other v1
	 * stage - see pipeline.c's own comment on why zero-copy is a later
	 * optimization, not a v1 concern. */
	memcpy(out->data, in->data, in->len);
	out->type = PORT_TYPE_VALIDATED;
	out->len = in->len;
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true };
}

void
validate_stage_teardown(void *state)
{
	free(state);
}
