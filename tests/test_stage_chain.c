#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/json.h"
#include "../src/record.h"
#include "../src/stages/validate_stage.h"
#include "../src/stages/extract_stage.h"
#include "../src/stages/convert_stage.h"
#include "../src/stages/dump_binary_stage.h"
#include "../src/stages/dump_text_stage.h"
#include "../src/pipeline.h"

/* DPDK-free: validate/extract/convert are all deliberately mbuf-free
 * (see stage.h's header comment), and pipeline.c's own chain-walking
 * logic is pure C with no DPDK/ring involvement either (see
 * pipeline.h) - this file exercises the individual stage functions
 * chained together by hand, and separately (see run_routing_test())
 * pipeline_run() itself against a small hand-built tree, without the
 * ring or forward_udp (the one stage type that actually needs DPDK)
 * involved at all. Run as a plain host binary, same convention as
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

/* --- Tiny hand-built router+leaves tree, exercising pipeline.c's
 * out_port-driven tree walk directly - no JSON, no graph_config.c
 * involved. Nothing else in this test suite proves pipeline_run()
 * actually follows stage_result.out_port to the right child rather
 * than always falling through to some fixed successor. */

static struct stage_result
router_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	(void)state;
	memcpy(out->data, in->data, in->len);
	out->len = in->len;
	out->type = PORT_TYPE_VALIDATED;
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true, .out_port = in->data[0] % 2 };
}

static unsigned
router_port_count(void *state)
{
	(void)state;
	return 2;
}

static const struct stage router_stage = {
	.name = "test_router",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD),
	.out_type = PORT_TYPE_VALIDATED,
	.out_port_count = router_port_count,
	.process = router_process,
};

static struct stage_result
leaf_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	bool *hit = state;
	*hit = true;
	memcpy(out->data, in->data, in->len);
	out->len = in->len;
	out->type = PORT_TYPE_WIRE_FRAME;
	out->capture_tsc = in->capture_tsc;
	return (struct stage_result){ .ok = true };
}

static unsigned
leaf_port_count(void *state)
{
	(void)state;
	return 0;
}

static const struct stage leaf_stage = {
	.name = "test_leaf",
	.in_types = PORT_TYPE_BIT(PORT_TYPE_VALIDATED),
	.out_type = PORT_TYPE_WIRE_FRAME,
	.out_port_count = leaf_port_count,
	.process = leaf_process,
};

static void
run_routing_test(void)
{
	bool leaf0_hit = false, leaf1_hit = false;

	struct pipeline_chain chain = {0};
	chain.root_idx = 0;
	chain.stage_count = 3;

	chain.stages[0].stage = &router_stage;
	chain.stages[0].port_count = 2;
	chain.stages[0].children[0] = 1;
	chain.stages[0].children[1] = 2;
	for (unsigned k = 2; k < STAGE_MAX_OUT_PORTS; k++)
		chain.stages[0].children[k] = -1;

	chain.stages[1].stage = &leaf_stage;
	chain.stages[1].state = &leaf0_hit;
	chain.stages[1].port_count = 0;
	for (unsigned k = 0; k < STAGE_MAX_OUT_PORTS; k++)
		chain.stages[1].children[k] = -1;

	chain.stages[2].stage = &leaf_stage;
	chain.stages[2].state = &leaf1_hit;
	chain.stages[2].port_count = 0;
	for (unsigned k = 0; k < STAGE_MAX_OUT_PORTS; k++)
		chain.stages[2].children[k] = -1;

	struct pipeline_worker worker;
	struct pipeline_counters counters;
	pipeline_counters_init(&counters);

	uint8_t even_payload[4] = { 0x04, 0, 0, 0 };
	assert(pipeline_run(&chain, &worker, &counters, even_payload, sizeof(even_payload), 0));
	assert(leaf0_hit && !leaf1_hit);
	assert(atomic_load(&counters.records_forwarded) == 1);
	printf("PASS: pipeline_run() routes an even-first-byte record to port 0's child\n");

	leaf0_hit = false;
	uint8_t odd_payload[4] = { 0x05, 0, 0, 0 };
	assert(pipeline_run(&chain, &worker, &counters, odd_payload, sizeof(odd_payload), 0));
	assert(leaf1_hit && !leaf0_hit);
	assert(atomic_load(&counters.records_forwarded) == 2);
	printf("PASS: pipeline_run() routes an odd-first-byte record to port 1's child, not port 0's\n");
}

/* dump_binary/dump_text are exercised directly (init/process/teardown,
 * no graph_config.c involved) then read back - the only place either
 * stage's actual file-writing behavior is verified. */
static void
run_dump_stage_tests(void)
{
	struct json_value *binary_cfg = parse_or_die("{\"path\": \"build/test_dump_binary.bin\"}");
	void *binary_state = dump_binary_stage_init(binary_cfg);
	assert(binary_state != NULL);

	uint8_t chunk1[3] = { 0x01, 0x02, 0x03 };
	struct stage_record bin_in1 = { .data = chunk1, .len = sizeof(chunk1) };
	struct stage_record bin_out1 = {0};
	assert(dump_binary_stage_process(binary_state, &bin_in1, &bin_out1).ok);

	uint8_t chunk2[2] = { 0xaa, 0xbb };
	struct stage_record bin_in2 = { .data = chunk2, .len = sizeof(chunk2) };
	struct stage_record bin_out2 = {0};
	assert(dump_binary_stage_process(binary_state, &bin_in2, &bin_out2).ok);

	dump_binary_stage_teardown(binary_state);

	FILE *bf = fopen("build/test_dump_binary.bin", "rb");
	assert(bf != NULL);
	uint8_t readback[5];
	assert(fread(readback, 1, sizeof(readback), bf) == sizeof(readback));
	fclose(bf);
	assert(memcmp(readback, "\x01\x02\x03\xaa\xbb", 5) == 0);
	printf("PASS: dump_binary_stage writes each record's bytes verbatim, back to back\n");

	struct json_value *text_cfg = parse_or_die("{\"path\": \"build/test_dump_text.txt\"}");
	void *text_state = dump_text_stage_init(text_cfg);
	assert(text_state != NULL);

	double value = 42.5;
	uint8_t value_bytes[8];
	memcpy(value_bytes, &value, sizeof(value));
	struct stage_record text_in = { .data = value_bytes, .len = sizeof(value_bytes) };
	struct stage_record text_out = {0};
	assert(dump_text_stage_process(text_state, &text_in, &text_out).ok);

	dump_text_stage_teardown(text_state);

	FILE *tf = fopen("build/test_dump_text.txt", "rb");
	assert(tf != NULL);
	char text_buf[64] = {0};
	size_t n = fread(text_buf, 1, sizeof(text_buf) - 1, tf);
	fclose(tf);
	assert(n > 0 && text_buf[n - 1] == '\r'); /* carriage return, not '\n' - see
						      dump_text_stage.c's own comment */
	assert(strstr(text_buf, "42.5") != NULL);
	printf("PASS: dump_text_stage writes an ASCII value with a trailing carriage return\n");
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

	run_routing_test();
	run_dump_stage_tests();

	printf("\nALL TESTS PASSED\n");
	return 0;
}
