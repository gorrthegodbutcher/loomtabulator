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

	/* 3, not 1: fixture_ok (one stage) plus plugin_loomlet.so's TWO
	 * stages (loom_stage_entry_at(), no loom_stage_entry() at all - see
	 * that fixture's own comment) - proves a single .so contributing
	 * more than one stage type actually works, not just compiles. */
	assert(stage_registry_count() == 3);
	printf("PASS: only the valid fixtures registered (bad-version and "
	       "missing-version fixtures were skipped, not fatal) - one plugin's "
	       "single stage plus a loomlet's two\n");

	assert(stage_registry_find("fixture_ok") != NULL);
	assert(stage_registry_find("fixture_loomlet_a") != NULL);
	assert(stage_registry_find("fixture_loomlet_b") != NULL);
	assert(stage_registry_find("fixture_badversion") == NULL);
	assert(stage_registry_find("fixture_missingversion") == NULL);
	printf("PASS: stage_registry_find() confirms exactly the valid fixtures are "
	       "registered (including both of the loomlet's stages), the two bad "
	       "ones are not\n");

	/* Load order is alphabetical by filename (scandir()+alphasort(), see
	 * plugin_loader.c) - "plugin_loomlet.so" sorts before "plugin_ok.so",
	 * so index 0 is the loomlet's FIRST stage now, not fixture_ok. */
	const struct stage *found = stage_registry_get(0);
	assert(found != NULL && strcmp(found->name, "fixture_loomlet_a") == 0);
	assert(found->process != NULL);
	printf("PASS: stage_registry_get(0) returns the same valid stage descriptor\n");

	/* The real point of this fixture: plugin_loomlet.so's two stages
	 * share ONE dlopen() handle (stored at two different g_handles[]
	 * slots) - plugin_loader_shutdown() dlclose()-ing it twice would be
	 * undefined behavior without the dedup fix in that function. This
	 * doesn't crash/abort is exactly what proves the fix works, not
	 * just that it compiles. */
	plugin_loader_shutdown();
	assert(stage_registry_count() == 0);
	printf("PASS: plugin_loader_shutdown() clears the registry (dlclose() of "
	       "every loaded handle didn't crash, including the loomlet's ONE "
	       "handle shared by two registered stages, closed exactly once)\n");

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
