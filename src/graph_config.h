#ifndef GRAPH_CONFIG_H
#define GRAPH_CONFIG_H

#include <stddef.h>
#include "pipeline.h"

/* v1's graph JSON schema is documented in full, with a worked example,
 * in the project plan (~/.claude/plans/noble-kindling-lemon.md). Short
 * version: {"nodes":[{"id","type","data":{"config":{...},"on_invalid"}}],
 * "edges":[{"source","target","source_port"|"invalid_target"}]} -
 * "source_port" is optional, defaulting to 0, so every single-output
 * graph (the vast majority) never needs to mention it. "on_invalid"
 * ("drop"|"pass", default "drop") and an edge's "invalid_target": true
 * are version-5 additions (see stage_abi.h's version history) governing
 * what happens to a record a node flags via
 * STAGE_RECORD_FLAG_INTEGRITY_FAILED - see pipeline.h's struct
 * pipeline_stage_instance comment for the full mechanism; also optional,
 * so an existing graph needs zero edits either way. The loader builds a
 * tree rooted at the one node with no incoming edge: fan-out (one node,
 * several outgoing edges on distinct source_port values, or one
 * additional invalid_target edge) is supported, but fan-in (a node with
 * more than one incoming edge, from any combination of these edge
 * kinds) is not - see graph_config.c's own validation, and stage.h's
 * struct stage.out_port_count comment for the ABI side of this.
 *
 * ring_name/ring_size out is what main.c uses to create (v1) or attach
 * to (Phase 4) the input rte_ring - kept here rather than hardcoded in
 * main.c so a graph file is fully self-describing.
 *
 * Takes a struct pipeline_chain (see pipeline.h) rather than a full
 * struct pipeline as of Phase 2 - this function only ever populates
 * stages[]/stage_count/root_idx, never per-worker scratch memory or
 * counters, so it stays exactly as single-threaded/startup-only as it
 * was in v1. */

struct graph_config_result {
	char ring_name[32];
	unsigned int ring_size;
};

/* Reads path, parses it as JSON, validates it against the schema above
 * (node types exist in plugin_loader.c's dynamically-populated
 * registry, every edge's source out_type is one of its target's
 * accepted in_types, the graph is a tree from a PORT_TYPE_RAW_RECORD-
 * input root to one or more leaves - a leaf's own out_type is
 * unconstrained, see stage.h's out_port_count comment), and on success
 * calls init()/out_port_count() on every stage
 * instance, populating pl->stages/stage_count/root_idx. On any failure,
 * writes a human-readable message into errbuf and returns false - this
 * is always a startup-time failure (see stage.h's own header comment on
 * why hot-path code has no comparable runtime error path), never
 * something pipeline_run() itself needs to handle. Any stage state
 * instances already init()'d before a later validation failure are
 * torn down before returning false. */
bool graph_config_load(const char *path, struct pipeline_chain *chain,
			struct graph_config_result *out, char *errbuf, size_t errbuf_len);

#endif
