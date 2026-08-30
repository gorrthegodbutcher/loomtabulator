#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include "../src/graph_config.h"

/* This tier links tests/stub_stage_registry.c (a frozen stand-in for
 * plugin_loader.c's dynamically-populated registry, plus a couple of
 * test-only multi-port stage doubles - see that file) instead of the
 * real dlopen() machinery - see the Makefile's own comment on why this
 * still runs as a plain host process with no EAL flags: nothing here
 * ever calls into EAL-dependent code, only forward_udp_stage_init()
 * (pure JSON parsing, no rte_* calls) - its process() is never invoked
 * by graph_config_load() or this test. */

/* A plain unique-path construction (pid + a counter) instead of
 * mkstemp() - sidesteps a POSIX feature-macro fight with this file's
 * CFLAGS (`-include rte_config.h` forces that header in before any
 * in-file #define could take effect - see the Makefile's own
 * -D_POSIX_C_SOURCE, which itself runs into the same header-order
 * issue). This is test-only code writing to a fixed, private-enough
 * path, not a security-sensitive temp file - no real need for
 * mkstemp()'s atomicity guarantees here. */
static const char *
write_temp_json(const char *content)
{
	static char path_buf[64];
	static int counter;
	snprintf(path_buf, sizeof(path_buf), "/tmp/loomtabulator_test_graph_%d_%d",
		 (int)getpid(), counter++);
	int fd = open(path_buf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	assert(fd >= 0);
	ssize_t written = write(fd, content, strlen(content));
	assert(written == (ssize_t)strlen(content));
	close(fd);
	return path_buf;
}

int
main(void)
{
	struct pipeline_chain pl;
	struct graph_config_result info;
	char errbuf[256];

	/* Valid v1 example graph */
	assert(graph_config_load("../testdata/example_graph.json", &pl, &info, errbuf, sizeof(errbuf)));
	assert(pl.stage_count == 4);
	assert(strcmp(info.ring_name, "LOOM_INPUT_RING") == 0);
	assert(info.ring_size == 4096);
	assert(strcmp(pl.stages[0].stage->name, "validate") == 0);
	assert(strcmp(pl.stages[1].stage->name, "extract") == 0);
	assert(strcmp(pl.stages[2].stage->name, "convert") == 0);
	assert(strcmp(pl.stages[3].stage->name, "dump_text") == 0);
	/* node_id (used by GET /api/stage-status - see pipeline.h) matches
	 * the graph JSON's own "id" strings exactly. */
	assert(strcmp(pl.stages[0].node_id, "n1") == 0);
	assert(strcmp(pl.stages[1].node_id, "n2") == 0);
	assert(strcmp(pl.stages[2].node_id, "n3") == 0);
	assert(strcmp(pl.stages[3].node_id, "n4") == 0);
	for (size_t i = 0; i < pl.stage_count; i++)
		if (pl.stages[i].stage != NULL && pl.stages[i].stage->teardown != NULL)
			pl.stages[i].stage->teardown(pl.stages[i].state);
	printf("PASS: loads the v1 example graph (4 stages, correct order)\n");

	/* Unknown stage type */
	const char *unknown_type =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":[{\"id\":\"n1\",\"type\":\"nonexistent_stage\"}],\"edges\":[]}";
	assert(!graph_config_load(write_temp_json(unknown_type), &pl, &info, errbuf, sizeof(errbuf)));
	printf("PASS: rejects unknown stage type (%s)\n", errbuf);

	/* Port-type mismatch: extract (expects raw_record) fed convert's
	 * engineering output directly - the type space collapsed to
	 * {raw_record, engineering, wire_frame} in version 5 (see
	 * stage_abi.h's version history), so "validated straight into
	 * convert" is no longer a mismatch at all (both are raw_record now)
	 * - engineering is the one type nothing byte-blob-shaped accepts. */
	const char *type_mismatch =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"extract\",\"data\":{\"config\":{\"field_offset_bytes\":0,\"field_width_bytes\":8}}},"
		"{\"id\":\"n3\",\"type\":\"convert\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n4\",\"type\":\"extract\",\"data\":{\"config\":{\"field_offset_bytes\":0,\"field_width_bytes\":8}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\"},{\"source\":\"n2\",\"target\":\"n3\"},"
		"{\"source\":\"n3\",\"target\":\"n4\"}]}";
	assert(!graph_config_load(write_temp_json(type_mismatch), &pl, &info, errbuf, sizeof(errbuf)));
	assert(strstr(errbuf, "doesn't accept an input of type 'engineering'") != NULL);
	assert(strstr(errbuf, "accepts: raw_record") != NULL);
	printf("PASS: rejects a port-type mismatch, listing the accepted type(s) (%s)\n", errbuf);

	/* validate straight into extract straight into forward_udp - all
	 * three now share the one raw_record type, so this needs neither
	 * a multi-bit in_types union nor extract's numeric mode to succeed. */
	const char *raw_record_chain =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"forward_udp\",\"data\":{\"config\":{\"dst_ip\":\"127.0.0.1\",\"dst_port\":1}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\"}]}";
	assert(graph_config_load(write_temp_json(raw_record_chain), &pl, &info, errbuf, sizeof(errbuf)));
	for (size_t i = 0; i < pl.stage_count; i++)
		if (pl.stages[i].stage != NULL && pl.stages[i].stage->teardown != NULL)
			pl.stages[i].stage->teardown(pl.stages[i].state);
	printf("PASS: validate feeds forward_udp directly - both raw_record now\n");

	/* ...but still rejects 'engineering' specifically (the one type
	 * that genuinely needs interpretation, not just a byte count - see
	 * stage.h's enum comment). */
	const char *engineering_rejected =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"extract\",\"data\":{\"config\":{\"field_offset_bytes\":0,\"field_width_bytes\":8}}},"
		"{\"id\":\"n3\",\"type\":\"convert\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n4\",\"type\":\"forward_udp\",\"data\":{\"config\":{\"dst_ip\":\"127.0.0.1\",\"dst_port\":1}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\"},{\"source\":\"n2\",\"target\":\"n3\"},"
		"{\"source\":\"n3\",\"target\":\"n4\"}]}";
	assert(!graph_config_load(write_temp_json(engineering_rejected), &pl, &info, errbuf, sizeof(errbuf)));
	assert(strstr(errbuf, "doesn't accept an input of type 'engineering'") != NULL);
	assert(strstr(errbuf, "accepts: raw_record") != NULL);
	printf("PASS: forward_udp still rejects 'engineering' specifically (%s)\n", errbuf);

	/* A leaf whose out_type isn't wire_frame (dump_text's is engineering)
	 * loads successfully - the old "every leaf must produce a wire
	 * frame" rule is gone now that there's more than one kind of
	 * terminal sink. */
	const char *non_wire_frame_leaf =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"extract\",\"data\":{\"config\":{\"field_offset_bytes\":0,\"field_width_bytes\":8}}},"
		"{\"id\":\"n3\",\"type\":\"convert\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n4\",\"type\":\"dump_text\",\"data\":{\"config\":{\"path\":\"build/test_graph_config_dump.txt\"}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\"},{\"source\":\"n2\",\"target\":\"n3\"},"
		"{\"source\":\"n3\",\"target\":\"n4\"}]}";
	assert(graph_config_load(write_temp_json(non_wire_frame_leaf), &pl, &info, errbuf, sizeof(errbuf)));
	for (size_t i = 0; i < pl.stage_count; i++)
		if (pl.stages[i].stage != NULL && pl.stages[i].stage->teardown != NULL)
			pl.stages[i].stage->teardown(pl.stages[i].state);
	printf("PASS: a leaf with a non-wire_frame out_type (dump_text) loads successfully\n");

	/* Two edges from the same single-output node's same (implicit,
	 * default) source_port - "validate" has no out_port_count, so it
	 * declares exactly 1 port via the NULL default, and a second edge
	 * from it collides on that same port 0. Branching itself is legal
	 * now (see the "wires a 2-port stage" tests below); this is
	 * specifically the duplicate-port case. */
	const char *duplicate_port =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"extract\",\"data\":{\"config\":{\"field_offset_bytes\":0,\"field_width_bytes\":8}}},"
		"{\"id\":\"n3\",\"type\":\"extract\",\"data\":{\"config\":{\"field_offset_bytes\":0,\"field_width_bytes\":8}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\"},{\"source\":\"n1\",\"target\":\"n3\"}]}";
	assert(!graph_config_load(write_temp_json(duplicate_port), &pl, &info, errbuf, sizeof(errbuf)));
	assert(strstr(errbuf, "already has an outgoing edge") != NULL);
	printf("PASS: rejects two edges from the same node's same output port (%s)\n", errbuf);

	/* Fan-in: two edges into the same node - still rejected, unrelated
	 * to output-port count (this build's engine is a tree, no merging
	 * upstream paths) */
	const char *fan_in =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"multi_out_stub\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"leaf_stub\",\"data\":{\"config\":{}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\",\"source_port\":0},"
		"{\"source\":\"n1\",\"target\":\"n2\",\"source_port\":1}]}";
	assert(!graph_config_load(write_temp_json(fan_in), &pl, &info, errbuf, sizeof(errbuf)));
	assert(strstr(errbuf, "more than one incoming edge") != NULL);
	printf("PASS: rejects fan-in - a node with more than one incoming edge (%s)\n", errbuf);

	/* Real branching graph: multi_out_stub (2 ports) wired to two
	 * distinct leaf_stub nodes - the actual positive case this whole
	 * feature is for. */
	const char *branching =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"multi_out_stub\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"leaf_stub\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n3\",\"type\":\"leaf_stub\",\"data\":{\"config\":{}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\",\"source_port\":0},"
		"{\"source\":\"n1\",\"target\":\"n3\",\"source_port\":1}]}";
	assert(graph_config_load(write_temp_json(branching), &pl, &info, errbuf, sizeof(errbuf)));
	assert(pl.stage_count == 3);
	assert(pl.stages[pl.root_idx].stage != NULL);
	assert(strcmp(pl.stages[pl.root_idx].stage->name, "multi_out_stub") == 0);
	assert(pl.stages[pl.root_idx].port_count == 2);
	assert(pl.stages[pl.root_idx].children[0] >= 0 &&
	       strcmp(pl.stages[pl.stages[pl.root_idx].children[0]].stage->name, "leaf_stub") == 0);
	assert(pl.stages[pl.root_idx].children[1] >= 0 &&
	       pl.stages[pl.root_idx].children[0] != pl.stages[pl.root_idx].children[1]);
	for (size_t i = 0; i < pl.stage_count; i++)
		if (pl.stages[i].stage != NULL && pl.stages[i].stage->teardown != NULL)
			pl.stages[i].stage->teardown(pl.stages[i].state);
	printf("PASS: loads a real branching graph (2-port stage wired to two distinct leaves)\n");

	/* multi_out_stub declares 2 ports but only port 0 is wired */
	const char *port_not_wired =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"multi_out_stub\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"leaf_stub\",\"data\":{\"config\":{}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\",\"source_port\":0}]}";
	assert(!graph_config_load(write_temp_json(port_not_wired), &pl, &info, errbuf, sizeof(errbuf)));
	assert(strstr(errbuf, "has no outgoing edge") != NULL);
	printf("PASS: rejects a stage that only wires some of its declared ports (%s)\n", errbuf);

	/* multi_out_stub declares 2 ports (0,1) but an edge wires port 2 */
	const char *port_out_of_range =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"multi_out_stub\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"leaf_stub\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n3\",\"type\":\"leaf_stub\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n4\",\"type\":\"leaf_stub\",\"data\":{\"config\":{}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\",\"source_port\":0},"
		"{\"source\":\"n1\",\"target\":\"n3\",\"source_port\":1},"
		"{\"source\":\"n1\",\"target\":\"n4\",\"source_port\":2}]}";
	assert(!graph_config_load(write_temp_json(port_out_of_range), &pl, &info, errbuf, sizeof(errbuf)));
	assert(strstr(errbuf, "only declares") != NULL);
	printf("PASS: rejects an edge wiring a port beyond what the stage declares (%s)\n", errbuf);

	/* Missing input.ring_name */
	const char *no_ring =
		"{\"input\":{},\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}}],\"edges\":[]}";
	assert(!graph_config_load(write_temp_json(no_ring), &pl, &info, errbuf, sizeof(errbuf)));
	printf("PASS: rejects a graph missing input.ring_name (%s)\n", errbuf);

	/* Version-5 "on_invalid"/"invalid_target": a node's normal edge and
	 * its one optional dedicated invalid-record edge are independent
	 * routing tables - both wired here, to two distinct leaves. */
	const char *invalid_routing =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"dump_binary\",\"data\":{\"config\":{\"path\":\"build/test_graph_config_normal.bin\"}}},"
		"{\"id\":\"n3\",\"type\":\"forward_udp\",\"data\":{\"config\":{\"dst_ip\":\"127.0.0.1\",\"dst_port\":3}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\"},"
		"{\"source\":\"n1\",\"target\":\"n3\",\"invalid_target\":true}]}";
	assert(graph_config_load(write_temp_json(invalid_routing), &pl, &info, errbuf, sizeof(errbuf)));
	assert(pl.stage_count == 3);
	assert(strcmp(pl.stages[pl.root_idx].stage->name, "validate") == 0);
	assert(pl.stages[pl.root_idx].children[0] >= 0 &&
	       strcmp(pl.stages[pl.stages[pl.root_idx].children[0]].stage->name, "dump_binary") == 0);
	assert(pl.stages[pl.root_idx].invalid_child >= 0 &&
	       strcmp(pl.stages[pl.stages[pl.root_idx].invalid_child].stage->name, "forward_udp") == 0);
	assert(pl.stages[pl.root_idx].pass_invalid == false); /* default, not requested here */
	for (size_t i = 0; i < pl.stage_count; i++)
		if (pl.stages[i].stage != NULL && pl.stages[i].stage->teardown != NULL)
			pl.stages[i].stage->teardown(pl.stages[i].state);
	printf("PASS: loads a graph wiring both a node's normal edge and its invalid-record edge\n");

	/* "on_invalid": "pass" is accepted (a real, distinct choice from the
	 * default "drop") */
	const char *on_invalid_pass =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{},\"on_invalid\":\"pass\"}},"
		"{\"id\":\"n2\",\"type\":\"dump_binary\",\"data\":{\"config\":{\"path\":\"build/test_graph_config_pass.bin\"}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\"}]}";
	assert(graph_config_load(write_temp_json(on_invalid_pass), &pl, &info, errbuf, sizeof(errbuf)));
	assert(pl.stages[pl.root_idx].invalid_child == -1);
	assert(pl.stages[pl.root_idx].pass_invalid == true);
	for (size_t i = 0; i < pl.stage_count; i++)
		if (pl.stages[i].stage != NULL && pl.stages[i].stage->teardown != NULL)
			pl.stages[i].stage->teardown(pl.stages[i].state);
	printf("PASS: \"on_invalid\": \"pass\" is accepted and resolved\n");

	/* An unrecognized on_invalid value is a startup error */
	const char *on_invalid_bad =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":[{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{},\"on_invalid\":\"quarantine\"}}],"
		"\"edges\":[]}";
	assert(!graph_config_load(write_temp_json(on_invalid_bad), &pl, &info, errbuf, sizeof(errbuf)));
	assert(strstr(errbuf, "must be \"drop\" or \"pass\"") != NULL);
	printf("PASS: rejects an unrecognized \"on_invalid\" value (%s)\n", errbuf);

	/* invalid_target and source_port are mutually exclusive on one edge */
	const char *invalid_target_with_source_port =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"dump_binary\",\"data\":{\"config\":{\"path\":\"build/test_graph_config_x.bin\"}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\",\"invalid_target\":true,\"source_port\":0}]}";
	assert(!graph_config_load(write_temp_json(invalid_target_with_source_port), &pl, &info, errbuf, sizeof(errbuf)));
	assert(strstr(errbuf, "mutually exclusive") != NULL);
	printf("PASS: rejects invalid_target combined with source_port on one edge (%s)\n", errbuf);

	/* A node can have at most one invalid-record edge */
	const char *two_invalid_targets =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"dump_binary\",\"data\":{\"config\":{\"path\":\"build/test_graph_config_y.bin\"}}},"
		"{\"id\":\"n3\",\"type\":\"dump_binary\",\"data\":{\"config\":{\"path\":\"build/test_graph_config_z.bin\"}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\",\"invalid_target\":true},"
		"{\"source\":\"n1\",\"target\":\"n3\",\"invalid_target\":true}]}";
	assert(!graph_config_load(write_temp_json(two_invalid_targets), &pl, &info, errbuf, sizeof(errbuf)));
	assert(strstr(errbuf, "already has an invalid-record edge") != NULL);
	printf("PASS: rejects a second invalid-record edge from the same node (%s)\n", errbuf);

	printf("\nALL TESTS PASSED\n");
	return 0;
}
