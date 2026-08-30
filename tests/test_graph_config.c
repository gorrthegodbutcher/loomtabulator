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
	assert(strcmp(pl.stages[3].stage->name, "forward_udp") == 0);
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

	/* Port-type mismatch: convert (expects EXTRACTED) directly after
	 * validate (produces VALIDATED) */
	const char *type_mismatch =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"convert\",\"data\":{\"config\":{}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\"}]}";
	assert(!graph_config_load(write_temp_json(type_mismatch), &pl, &info, errbuf, sizeof(errbuf)));
	printf("PASS: rejects a port-type mismatch (%s)\n", errbuf);

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

	printf("\nALL TESTS PASSED\n");
	return 0;
}
