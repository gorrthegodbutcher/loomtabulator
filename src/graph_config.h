#ifndef GRAPH_CONFIG_H
#define GRAPH_CONFIG_H

#include <stddef.h>
#include "pipeline.h"

/* v1's graph JSON schema is documented in full, with a worked example,
 * in the project plan (~/.claude/plans/noble-kindling-lemon.md). Short
 * version: {"nodes":[{"id","type","data":{"config":{...}}}],
 * "edges":[{"source","target"}]}. The schema itself allows branching (so
 * a future Phase 2/3 graph doesn't need a version bump), but v1's loader
 * requires the graph to be a single linear chain and rejects anything
 * else - see graph_config.c's own validation.
 *
 * ring_name/ring_size out is what main.c uses to create (v1) or attach
 * to (Phase 4) the input rte_ring - kept here rather than hardcoded in
 * main.c so a graph file is fully self-describing. */

struct graph_config_result {
	char ring_name[32];
	unsigned int ring_size;
};

/* Reads path, parses it as JSON, validates it against the schema above
 * (node types exist in stage_registry.c, every edge's port types match,
 * the graph is a single linear chain from a PORT_TYPE_RAW_RECORD-input
 * node to a PORT_TYPE_WIRE_FRAME-output node), and on success calls
 * init() on every stage instance in chain order, populating pl->stages/
 * stage_count. On any failure, writes a human-readable message into
 * errbuf and returns false - this is always a startup-time failure (see
 * stage.h's own header comment on why hot-path code has no comparable
 * runtime error path), never something pipeline_run() itself needs to
 * handle. Any stage state instances already init()'d before a later
 * validation failure are torn down before returning false. */
bool graph_config_load(const char *path, struct pipeline *pl,
			struct graph_config_result *out, char *errbuf, size_t errbuf_len);

#endif
