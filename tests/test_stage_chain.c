#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/json.h"
#include "../src/record.h"
#include "../src/stages/validate_stage.h"
#include "../src/stages/extract_stage.h"
#include "../src/stages/convert_stage.h"

/* DPDK-free: validate/extract/convert are all deliberately mbuf-free
 * (see stage.h's header comment) - this exercises them chained together
 * exactly as pipeline.c would, without pipeline.c, the ring, or
 * forward_udp (the one stage type that actually needs DPDK) involved at
 * all. Run as a plain host binary, same convention as
 * dpdk-app-example's test_common.c. */

static struct json_value *
parse_or_die(const char *text)
{
	/* Heap-allocated, deliberately never freed - json_parse()'s own
	 * contract (json.h) requires the source text to outlive the
	 * returned tree (string values, including object keys, point
	 * directly into it, no copying). A stack-local buffer here would
	 * die the moment this function returns while the tree's keys still
	 * pointed into it - real corruption caught live in this exact spot
	 * while building this test. graph_config.c's own read_whole_file()
	 * already gets this right, for the same reason. */
	char *buf = strdup(text);
	char errbuf[128];
	struct json_value *v = json_parse(buf, errbuf, sizeof(errbuf));
	if (v == NULL) {
		fprintf(stderr, "test config JSON failed to parse: %s\n", errbuf);
		abort();
	}
	return v;
}

static void
build_record(uint8_t *buf, uint64_t magic, uint64_t raw_value, uint32_t payload_len)
{
	struct chrono_record_hdr *hdr = (struct chrono_record_hdr *)buf;
	hdr->magic = magic;
	hdr->seq = 0;
	hdr->capture_tsc = 123456789;
	hdr->len = payload_len;
	hdr->reserved = 0;

	uint8_t *payload = buf + sizeof(*hdr);
	memset(payload, 0, payload_len);
	for (int i = 7; i >= 0; i--) {
		payload[i] = (uint8_t)raw_value;
		raw_value >>= 8;
	}
}

int
main(void)
{
	uint8_t record[64];
	build_record(record, CHRONO_RECORD_MAGIC, 42, 16);

	/* Happy path: validate -> extract -> convert, chained by hand.
	 * Config trees are parsed into named variables first, not passed
	 * straight into init() as a nested call - keeps each config's
	 * lifetime and value unambiguous at every call site, and sidesteps
	 * an observed GCC codegen quirk with this exact
	 * function-returning-a-heap-pointer-from-a-stack-buffer-parse
	 * nested directly into another call, on this toolchain. */
	struct json_value *validate_cfg = parse_or_die("{\"require_magic\": true}");
	void *validate_state = validate_stage_init(validate_cfg);
	assert(validate_state != NULL);

	struct json_value *extract_cfg =
		parse_or_die("{\"field_offset_bytes\": 0, \"field_width_bytes\": 8}");
	void *extract_state = extract_stage_init(extract_cfg);
	assert(extract_state != NULL);

	struct json_value *convert_cfg = parse_or_die("{\"scale\": 0.001, \"offset\": 0.0}");
	void *convert_state = convert_stage_init(convert_cfg);
	assert(convert_state != NULL);

	struct stage_record raw = { .type = PORT_TYPE_RAW_RECORD, .data = record,
				     .len = sizeof(struct chrono_record_hdr) + 16,
				     .capture_tsc = 123456789 };
	uint8_t buf1[256], buf2[256], buf3[256];
	struct stage_record validated = { .data = buf1 };
	struct stage_result r1 = validate_stage_process(validate_state, &raw, &validated);
	assert(r1.ok);
	assert(validated.type == PORT_TYPE_VALIDATED);
	assert(validated.capture_tsc == 123456789);

	struct stage_record extracted = { .data = buf2 };
	struct stage_result r2 = extract_stage_process(extract_state, &validated, &extracted);
	assert(r2.ok);
	assert(extracted.type == PORT_TYPE_EXTRACTED);
	assert(extracted.len == 8);

	struct stage_record engineering = { .data = buf3 };
	struct stage_result r3 = convert_stage_process(convert_state, &extracted, &engineering);
	assert(r3.ok);
	assert(engineering.type == PORT_TYPE_ENGINEERING);
	double value;
	memcpy(&value, engineering.data, sizeof(value));
	assert(value > 0.0419 && value < 0.0421); /* 42 * 0.001 */
	printf("PASS: validate -> extract -> convert chain (42 raw -> %.4f engineering)\n", value);

	/* validate rejects bad magic */
	uint8_t bad_magic[64];
	build_record(bad_magic, 0xdeadbeef, 42, 16);
	struct stage_record raw2 = { .data = bad_magic, .len = sizeof(struct chrono_record_hdr) + 16 };
	struct stage_record out2 = { .data = buf1 };
	assert(!validate_stage_process(validate_state, &raw2, &out2).ok);
	printf("PASS: validate rejects bad magic\n");

	/* validate rejects a record shorter than the header */
	struct stage_record too_short = { .data = record, .len = 4 };
	assert(!validate_stage_process(validate_state, &too_short, &out2).ok);
	printf("PASS: validate rejects undersized record\n");

	/* validate rejects hdr->len mismatched with actual record length */
	uint8_t bad_len[64];
	build_record(bad_len, CHRONO_RECORD_MAGIC, 42, 16);
	struct stage_record raw3 = { .data = bad_len, .len = sizeof(struct chrono_record_hdr) + 8 /* wrong */ };
	assert(!validate_stage_process(validate_state, &raw3, &out2).ok);
	printf("PASS: validate rejects hdr->len/record-size mismatch\n");

	/* extract rejects a field that falls outside the payload */
	struct json_value *extract_oob_cfg =
		parse_or_die("{\"field_offset_bytes\": 12, \"field_width_bytes\": 8}");
	void *extract_oob_state = extract_stage_init(extract_oob_cfg);
	assert(extract_oob_state != NULL);
	struct stage_record oob_out = { .data = buf2 };
	assert(!extract_stage_process(extract_oob_state, &validated, &oob_out).ok);
	printf("PASS: extract rejects out-of-range field\n");

	/* extract_stage_init rejects an invalid field width */
	struct json_value *bad_width_cfg =
		parse_or_die("{\"field_offset_bytes\": 0, \"field_width_bytes\": 3}");
	assert(extract_stage_init(bad_width_cfg) == NULL);
	printf("PASS: extract_stage_init rejects invalid field_width_bytes\n");

	/* convert with a different scale/offset */
	struct json_value *convert2_cfg = parse_or_die("{\"scale\": 2.0, \"offset\": 100.0}");
	void *convert2_state = convert_stage_init(convert2_cfg);
	assert(convert2_state != NULL);
	struct stage_record eng2 = { .data = buf3 };
	assert(convert_stage_process(convert2_state, &extracted, &eng2).ok);
	double value2;
	memcpy(&value2, eng2.data, sizeof(value2));
	assert(value2 > 183.9 && value2 < 184.1); /* 42 * 2.0 + 100.0 = 184 */
	printf("PASS: convert applies scale/offset correctly (42 -> %.1f)\n", value2);

	validate_stage_teardown(validate_state);
	extract_stage_teardown(extract_state);
	extract_stage_teardown(extract_oob_state);
	convert_stage_teardown(convert_state);
	convert_stage_teardown(convert2_state);

	printf("\nALL TESTS PASSED\n");
	return 0;
}
