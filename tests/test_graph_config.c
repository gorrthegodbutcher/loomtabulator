#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include "../src/graph_config.h"

/* graph_config.c pulls in stage_registry.c (for its forward_udp table
 * entry, DPDK-touching) - see the Makefile's own comment on why this
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
		if (pl.stages[i].stage->teardown != NULL)
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

	/* Branching graph (one node with two outgoing edges) - not a linear
	 * chain, v1 must reject it */
	const char *branching =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}},"
		"{\"id\":\"n2\",\"type\":\"extract\",\"data\":{\"config\":{\"field_offset_bytes\":0,\"field_width_bytes\":8}}},"
		"{\"id\":\"n3\",\"type\":\"extract\",\"data\":{\"config\":{\"field_offset_bytes\":0,\"field_width_bytes\":8}}}"
		"],"
		"\"edges\":[{\"source\":\"n1\",\"target\":\"n2\"},{\"source\":\"n1\",\"target\":\"n3\"}]}";
	assert(!graph_config_load(write_temp_json(branching), &pl, &info, errbuf, sizeof(errbuf)));
	printf("PASS: rejects a branching (non-chain) graph (%s)\n", errbuf);

	/* Multi-output stage (max_out_ports > 1) - graph_config.c's engine
	 * only executes single-output chains today, so this must be
	 * rejected at load time rather than silently misrouted at runtime */
	const char *multi_out =
		"{\"input\":{\"ring_name\":\"R\"},"
		"\"nodes\":[{\"id\":\"n1\",\"type\":\"multi_out_stub\",\"data\":{\"config\":{}}}],"
		"\"edges\":[]}";
	assert(!graph_config_load(write_temp_json(multi_out), &pl, &info, errbuf, sizeof(errbuf)));
	assert(strstr(errbuf, "multi-port routing isn't executable yet") != NULL);
	printf("PASS: rejects a stage declaring more than one output port (%s)\n", errbuf);

	/* Missing input.ring_name */
	const char *no_ring =
		"{\"input\":{},\"nodes\":["
		"{\"id\":\"n1\",\"type\":\"validate\",\"data\":{\"config\":{}}}],\"edges\":[]}";
	assert(!graph_config_load(write_temp_json(no_ring), &pl, &info, errbuf, sizeof(errbuf)));
	printf("PASS: rejects a graph missing input.ring_name (%s)\n", errbuf);

	printf("\nALL TESTS PASSED\n");
	return 0;
}
