#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include "stage.h"

/* The stage registry - now backed by dynamically dlopen()'d .so
 * plugins instead of a compile-time array (see stage_abi.h for the
 * ABI contract every plugin must satisfy, and this file's own .c for
 * the loading protocol). Every built-in stage (validate/extract/
 * convert/forward_udp) is rebuilt as a plugin too and loaded through
 * this exact same mechanism - there is no separate "built-in" code
 * path, on purpose, so the plugin ABI is always dogfooded by this
 * project's own stages, never a second-class path only third parties
 * use.
 *
 * A .so need not be exactly one stage type - a "loomlet" (stage_abi.h's
 * own term) bundles several related stage types into one plugin via
 * loom_stage_entry_at(index) instead of the ordinary single-stage
 * loom_stage_entry(). This is transparent below the registry: each
 * stage a loomlet contributes gets its own g_registry[]/g_handles[]
 * slot exactly like a single-stage plugin's one stage would, so
 * everything downstream of plugin_loader_load() (graph_config.c,
 * web_status.c) never needs to know or care how many .so files
 * actually produced the stages it sees.
 *
 * The three query functions below are unchanged from the pre-plugin
 * design (same names, same signatures) - graph_config.c and
 * web_status.c need zero changes beyond the #include, since neither
 * ever assumed the backing store was compile-time-static, only that
 * it's populated before use (see plugin_loader_load()). */

#define PLUGIN_REGISTRY_MAX 64  /* distinct stage TYPES that can be
				  * registered - NOT the same as the number
				  * of .so files (a loomlet contributes more
				  * than one stage type from a single .so) -
				  * and unrelated to pipeline.h's
				  * PIPELINE_MAX_STAGES, which bounds NODES in
				  * one graph's chain, not how many stage
				  * types exist at all. */

/* Looks up a stage type by name (matches a graph JSON node's "type"
 * field, case-sensitive). Returns NULL if no such stage type was
 * successfully loaded - graph_config.c treats that as a startup-time
 * error, never a runtime one. */
const struct stage *stage_registry_find(const char *name);

/* The whole table, indexable - GET /api/stage-types (web_status.c)
 * serializes all of these so the web UI's palette and edge-validation
 * rules derive from what actually loaded, not a client-side list. */
size_t stage_registry_count(void);
const struct stage *stage_registry_get(size_t idx);

/* String form of a port type - see stage_registry_count()'s comment;
 * the web UI compares these strings directly rather than inventing its
 * own type vocabulary. */
const char *stage_port_type_name(enum stage_port_type type);

/* Scans plugins_dir for *.so files (deterministic order - see
 * plugin_loader.c) and dlopen()s every one found, checking each
 * against stage_abi.h's ABI contract before registering it. NULL or
 * "" plugins_dir, or a directory that doesn't exist/can't be read, is
 * NOT an error - it just means zero plugins loaded (matches
 * web_status.h's --web-root="" empty-string-disables convention) -
 * main.c should still call this even when running with no plugins
 * directory at all, since a graph with zero nodes would still validly
 * load (rejected later by graph_config.c's own "no nodes" check, not
 * this function's).
 *
 * A genuine failure - a stage-name collision (two plugins, or a
 * plugin re-declaring a built-in's name; no special-casing, built-ins
 * load through this exact path) or more than PLUGIN_REGISTRY_MAX
 * stage types found - returns false with a human-readable message in
 * errbuf; main.c treats that as a refuse-to-run startup failure, same
 * posture as a bad --graph=PATH file. An individual plugin that fails
 * to load cleanly for its own reasons (dlopen error, ABI version
 * mismatch, missing export, NULL/invalid descriptor) is logged to
 * stderr and skipped, NOT treated as fatal to the whole scan - only
 * running out of registry slots or a name collision aborts startup,
 * since those affect graphs that reference OTHER, perfectly good
 * plugins too. */
bool plugin_loader_load(const char *plugins_dir, char *errbuf, size_t errbuf_len);

/* dlclose()s every loaded plugin handle, in reverse load order. Call
 * ONLY after every stage instance's own teardown() has already run
 * (main.c does - see its shutdown sequence) and after every worker
 * lcore/thread that could call into a stage's process() has already
 * been joined (rte_eal_mp_wait_lcore()/pthread_join() have already
 * returned by main.c's call site) - dlclose()-ing a plugin a live
 * worker might still call into would be a real use-after-unmap bug.
 * Safe to call even if plugin_loader_load() was never called or
 * loaded zero plugins. */
void plugin_loader_shutdown(void);

#endif
