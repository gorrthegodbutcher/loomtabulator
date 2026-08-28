#ifndef STAGE_H
#define STAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "json.h"

/* Upper bound on any single stage_record's data, in either direction -
 * pipeline.c allocates scratch buffers this size (see pipeline.c's
 * ping-pong buffer pair), and every stage's process() can assume its
 * `out` buffer has this much room without checking. Matches this
 * project family's MBUF_DATA_SIZE convention (main.c/interactive_sender.c
 * in dpdk-app-example) - generous enough for a jumbo raw record, way
 * more than extract/convert's own small fixed-width outputs need. */
#define STAGE_SCRATCH_BYTES 9216

/* The pipeline processing contract - see the project plan
 * (~/.claude/plans/noble-kindling-lemon.md) for the full design
 * rationale. A "stage" is one step in a linear chain (validate ->
 * extract -> convert -> forward in the v1 example graph, but the
 * mechanism itself doesn't hardcode those four - stage_registry.c is
 * just a compile-time table of whatever stage types exist, and
 * graph_config.c builds an arbitrary ordered chain from it).
 *
 * Deliberately mbuf-free and DPDK-independent, same discipline as
 * common.c: a stage_record is plain memory, not an rte_mbuf. Only the
 * forward_udp stage (the one stage type that actually transmits)
 * touches DPDK/mbuf APIs at all - every other stage type, and the
 * pipeline-chaining logic itself, can be unit-tested as a host binary
 * with no hugepages/EAL/hardware involved. See tests/test_stage_chain.c. */

/* Small, closed set on purpose (not an open/string type system) so
 * connecting two stages is a simple equality check - both here, in
 * graph_config.c's edge validation, and eventually in the web UI's
 * connection rules (Phase 3), which will just serialize this same
 * enum's names from stage_registry.c's table rather than invent its own
 * type system. */
enum stage_port_type {
	PORT_TYPE_RAW_RECORD,   /* struct chrono_record_hdr + payload, exactly
				  * as read off the input ring - nothing checked
				  * yet. validate's input type. */
	PORT_TYPE_VALIDATED,    /* same wire shape as RAW_RECORD, but the
				  * validate stage has already confirmed magic
				  * and len are sane - downstream stages can
				  * assume that without re-checking. */
	PORT_TYPE_EXTRACTED,    /* a stage-defined fixed-layout value pulled
				  * out of the payload (see extract_stage.c) -
				  * opaque bytes outside of that stage's own
				  * config-declared offset/width. */
	PORT_TYPE_ENGINEERING,  /* a raw extracted value converted to
				  * engineering units (see convert_stage.c) -
				  * a little-endian IEEE 754 double, always,
				  * regardless of the raw field's original
				  * width, so every stage downstream of convert
				  * has one fixed value shape to deal with. */
	PORT_TYPE_WIRE_FRAME,   /* a fully-built outbound UDP frame, ready to
				  * hand to app_build_packet()'s caller for
				  * transmission - forward_udp's input type,
				  * and the only type forward_udp ever expects
				  * to receive (it does the building itself;
				  * this type exists so a graph can be
				  * type-checked even though v1 always has
				  * forward_udp build the frame from
				  * PORT_TYPE_ENGINEERING - see forward_udp_stage.c's
				  * own comment on why its declared in_type is
				  * PORT_TYPE_ENGINEERING, not this one, and
				  * this type is reserved for a future stage
				  * that builds a frame explicitly). */
};

/* Carried through every stage in the chain unchanged, alongside whatever
 * that stage's own process() actually transforms. capture_tsc in
 * particular has no meaning to most stages individually but is real
 * signal a much later consumer (Phase 2's epoch barrier, forward_udp's
 * optional --preserve-timing pacing, a future diagnostic stage) needs -
 * simplest to just always carry it rather than have every stage type
 * decide whether it cares. */
struct stage_record {
	enum stage_port_type type;
	uint8_t *data;    /* plain heap/stack memory owned by the pipeline
			    * runner, NOT an rte_mbuf - see this file's own
			    * header comment. Points into a per-record scratch
			    * buffer sized generously enough for any stage's
			    * output (see pipeline.h's STAGE_SCRATCH_BYTES). */
	uint32_t len;
	uint64_t capture_tsc;
};

/* A stage's outcome for one record. ok=false means "drop this record,
 * don't pass it to the next stage" - this is validate's ordinary,
 * expected behavior for malformed input, not a fatal error, so
 * pipeline.c counts it and continues rather than aborting the whole
 * run. There is deliberately no separate "error" outcome here: matching
 * this project family's convention of pushing failure modes to
 * startup-time validation wherever possible (see graph_config.c) rather
 * than inventing hot-path error paths a stage's own inputs already rule
 * out - if a stage's config was accepted at init() time, its process()
 * is expected to always produce a definite ok=true or ok=false result,
 * never fail in some third way. */
struct stage_result {
	bool ok;
	const char *drop_reason; /* NULL if ok; always a string literal, never
				    * heap-allocated - this runs on the hot path
				    * once per record. */
};

/* One stage type's full contract. stage_registry.c holds a fixed,
 * compile-time array of these (name -> the rest of the tuple) - this is
 * intentionally NOT a dynamically loaded plugin table (no dlopen, no
 * .so), matching how chrontabulator statically links its one PMD rather
 * than loading it at runtime: every stage type this binary can ever run
 * is known at compile time, which is also what lets a JSON graph
 * referencing an unknown stage type fail cleanly at startup (see
 * graph_config.c) instead of needing a runtime module-loading story. */
struct stage {
	const char *name;         /* matches a graph JSON node's "type" field
				     * exactly (see graph_config.c) */
	enum stage_port_type in_type;
	enum stage_port_type out_type;

	/* config is this node's "data.config" object straight out of the
	 * parsed graph JSON (see graph_config.c) - each stage type pulls out
	 * whatever fields it needs by name (json_object_get() + json_as_*()),
	 * same "read only what you need, fall back on absence" convention
	 * query_get_uint()/query_get_str() already use for the web servers'
	 * query strings elsewhere in this project family. Returns an opaque
	 * pointer process()/teardown() receive back unchanged (e.g. convert's
	 * parsed scale/offset coefficients), or NULL on a bad config -
	 * graph_config.c treats NULL as a startup-time failure (refuse to
	 * run, clear error), never something discovered mid-run. */
	void *(*init)(const struct json_value *config);

	/* The hot-path function, called once per record that reaches this
	 * stage. in->type is always this stage's declared in_type (the
	 * pipeline runner guarantees that by construction, from graph
	 * validation - a stage's process() never needs to check it itself).
	 * On ok=true, the caller expects *out to be fully populated
	 * (type=this stage's out_type, data/len/capture_tsc set) and passes
	 * it on to the next stage; on ok=false, *out is ignored. */
	struct stage_result (*process)(void *state, const struct stage_record *in,
					struct stage_record *out);

	/* Mirror of init() - called once per stage instance at pipeline
	 * shutdown. NULL state (init() returned NULL for some reason it
	 * treated as non-fatal, or this stage type has no state at all) must
	 * be handled gracefully - not every stage needs teardown, but every
	 * stage struct has the pointer for uniformity. */
	void (*teardown)(void *state);
};

#endif
