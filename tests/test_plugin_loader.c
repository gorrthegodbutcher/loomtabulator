#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../src/plugin_loader.h"

/* Exercises the real dlopen()/dlsym()/ABI-check/collision logic in
 * src/plugin_loader.c - the one piece of the Phase-3-plugin conversion
 * that tests/test_graph_config.c's frozen tests/stub_stage_registry.c
 * deliberately does NOT cover (that tier tests graph_config.c's
 * chain-building against a known-good registry; this tier tests how
 * the registry itself gets populated). DPDK-free, same posture as
 * test_stage_chain.c/test_graph_config.c - the Makefile builds the
 * fixture plugins in tests/plugin_fixtures/ into two throwaway
 * directories (never the real plugins/) before this binary runs. */

#define OK_FIXTURES_DIR "build/test_plugin_fixtures_ok"
#define COLLISION_FIXTURES_DIR "build/test_plugin_fixtures_collision"

int
main(void)
{
	char errbuf[256];

	/* --- Success + per-plugin-rejection path --- */
	bool ok = plugin_loader_load(OK_FIXTURES_DIR, errbuf, sizeof(errbuf));
	assert(ok);
	printf("PASS: plugin_loader_load() succeeds on a directory with one valid "
	       "plugin and two individually-bad ones\n");

	assert(stage_registry_count() == 1);
	printf("PASS: only the valid fixture registered (bad-version and "
	       "missing-version fixtures were skipped, not fatal)\n");

	assert(stage_registry_find("fixture_ok") != NULL);
	assert(stage_registry_find("fixture_badversion") == NULL);
	assert(stage_registry_find("fixture_missingversion") == NULL);
	printf("PASS: stage_registry_find() confirms exactly the valid fixture is "
	       "registered, the two bad ones are not\n");

	const struct stage *found = stage_registry_get(0);
	assert(found != NULL && strcmp(found->name, "fixture_ok") == 0);
	assert(found->process != NULL);
	printf("PASS: stage_registry_get(0) returns the same valid stage descriptor\n");

	plugin_loader_shutdown();
	assert(stage_registry_count() == 0);
	printf("PASS: plugin_loader_shutdown() clears the registry (dlclose() of "
	       "every loaded handle didn't crash)\n");

	/* --- Empty/disabled plugins_dir: not an error --- */
	errbuf[0] = '\0';
	ok = plugin_loader_load("", errbuf, sizeof(errbuf));
	assert(ok && stage_registry_count() == 0);
	ok = plugin_loader_load(NULL, errbuf, sizeof(errbuf));
	assert(ok && stage_registry_count() == 0);
	printf("PASS: empty/NULL plugins_dir loads zero plugins, not an error\n");

	/* --- Missing directory: not an error, matches --web-root's posture --- */
	ok = plugin_loader_load("build/this_directory_does_not_exist", errbuf, sizeof(errbuf));
	assert(ok && stage_registry_count() == 0);
	printf("PASS: a nonexistent plugins directory loads zero plugins, not fatal\n");

	/* --- Name collision: fatal --- */
	ok = plugin_loader_load(COLLISION_FIXTURES_DIR, errbuf, sizeof(errbuf));
	assert(!ok);
	assert(strstr(errbuf, "fixture_dup") != NULL);
	printf("PASS: a stage-name collision between two plugins is rejected "
	       "(fatal, clear errbuf: \"%s\")\n", errbuf);

	printf("\nALL TESTS PASSED\n");
	return 0;
}
