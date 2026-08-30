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

/* Structural cap on struct stage.out_port_count()'s return value - array
 * sizing (pipeline.h's struct pipeline_stage_instance.children[]), not a
 * real limit on any stage's own routing table; bump if a real plugin
 * needs more. Lives here rather than pipeline.h since it bounds a valid
 * out_port_count() return value - an ABI-facing contract every plugin
 * author needs to know, not an internal engine detail. */
#define STAGE_MAX_OUT_PORTS 16

/* Structural caps on struct stage.get_status()'s output - array/string
 * sizing (see struct stage_status below), not a real limit on how much
 * a stage could conceivably want to report; bump if a real plugin needs
 * more. */
#define STAGE_MAX_STATUS_FIELDS 8
#define STAGE_STATUS_NAME_MAX 32

/* One named counter a stage reports via get_status() below - e.g.
 * {"records_checked", 1042}. Fixed-size name (not a pointer) so this
 * struct is fully self-contained and safely copyable by value across
 * the ABI boundary, with no lifetime/ownership question the way a
 * returned string pointer would raise - same discipline struct
 * stage_record/struct stage_result already use. */
struct stage_status_field {
	char name[STAGE_STATUS_NAME_MAX];
	uint64_t value;
};

/* A stage instance's current status snapshot - see struct stage's own
 * get_status comment for when/how this gets filled. field_count == 0
 * is a completely normal answer (this stage has nothing to report right
 * now, or ever), not an error. Values are plain counters (uint64) for
 * now - deliberately not a richer typed value (a string state, a float
 * average): every example this was designed against (total/passed/
 * failed message counts) is a counter, and widening to other types
 * later is a small additive change if a real need shows up, not
 * something worth building speculatively today. */
struct stage_status {
	struct stage_status_field fields[STAGE_MAX_STATUS_FIELDS];
	unsigned field_count;
};

/* Turns an enum stage_port_type value into its bit for struct
 * stage.in_types below - a stage accepting several input types ORs
 * these together (e.g. PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD) |
 * PORT_TYPE_BIT(PORT_TYPE_WIRE_FRAME), dump_binary's own combination).
 * The type space is small (3 values today) so this comfortably fits one
 * unsigned int; matches this file's own STAGE_RECORD_FLAG_* bit-flag
 * convention. */
#define PORT_TYPE_BIT(t) (1u << (unsigned)(t))

/* The pipeline processing contract - see the project plan
 * (~/.claude/plans/noble-kindling-lemon.md) for the full design
 * rationale. A "stage" is one step in a linear chain (validate ->
 * extract -> convert -> forward in the v1 example graph, but the
 * mechanism itself doesn't hardcode those four - plugin_loader.c
 * builds a dynamic table of whatever stage types were successfully
 * loaded as .so plugins, and graph_config.c builds an arbitrary
 * ordered chain from it).
 *
 * This struct, together with stage_abi.h's version contract, IS the
 * plugin ABI surface - every stage type (built-in or third-party) is a
 * dlopen()'d .so exporting one of these (see plugin_loader.c/
 * stage_abi.h for the full loading protocol). Any layout or meaning
 * change here requires bumping stage_abi.h's STAGE_ABI_VERSION.
 *
 * Deliberately mbuf-free and DPDK-independent, same discipline as
 * common.c: a stage_record is plain memory, not an rte_mbuf. No stage
 * type touches DPDK/mbuf APIs at all anymore (forward_udp sends over a
 * plain kernel UDP socket) - this whole file, and json.h alongside it,
 * are the complete surface a plugin author needs, with zero DPDK
 * dependency, and the pipeline-chaining logic itself can be
 * unit-tested as a host binary with no hugepages/EAL/hardware
 * involved. See tests/test_stage_chain.c. */

/* Small, closed set on purpose (not an open/string type system) so
 * connecting two stages is a simple membership check (is the upstream
 * out_type one of the downstream stage's declared in_types? - see
 * struct stage.in_types below) - both here, in graph_config.c's edge
 * validation, and in the web UI's connection rules (Phase 3), which
 * serialize this same enum's names from plugin_loader.c's
 * dynamically-populated table rather than invent its own type
 * system. */
/* Version 5 collapsed what used to be four separate "bytes" shapes
 * (raw_record/validated/extracted) into this one value - see
 * stage_abi.h's version history for the full reasoning. Whether a given
 * PORT_TYPE_RAW_RECORD record's header has been checked (validate) or
 * its payload has been narrowed to a sub-slice (extract, either mode)
 * is NOT encoded in the type anymore - the type only ever describes
 * shape ("this is an opaque byte blob"), never trust/quality. That's
 * struct stage_record.flags's job (STAGE_RECORD_FLAG_INTEGRITY_FAILED
 * below, and whatever future per-check flags a stage wants to add) -
 * "has this record passed check X" is an orthogonal, stackable
 * attribute of the data, not a different kind of data. For the common
 * case, chain POSITION alone already tells you what's been checked
 * (nothing reaches convert without having gone through extract, exactly
 * the same way nothing reaches extract without going through validate -
 * enforced by in_types membership, not by a dedicated type per stage). */
enum stage_port_type {
	PORT_TYPE_RAW_RECORD,   /* struct chrono_record_hdr + payload, or any
				  * stage-narrowed sub-slice of one (see
				  * extract_stage.c) - an opaque byte blob,
				  * validated or not, sliced or not. validate's
				  * input AND output type; extract's input AND
				  * output type (both modes); forward_udp/
				  * dump_binary's accepted input. */
	PORT_TYPE_ENGINEERING,  /* a raw extracted value converted to
				  * engineering units (see convert_stage.c) -
				  * a little-endian IEEE 754 double, always,
				  * regardless of the raw field's original
				  * width, so every stage downstream of convert
				  * has one fixed value shape to deal with. The
				  * one type that genuinely needs interpretation
				  * (not just a byte count) to make sense of. */
	PORT_TYPE_WIRE_FRAME,   /* forward_udp's OUTPUT type - a record that's
				  * already been built and handed off for
				  * transmission (or, for dump_binary, already
				  * written to a file) - not something any
				  * stage declares as an *input* type today.
				  * Also one of dump_binary_stage.c's several
				  * accepted input types, for capturing exactly
				  * what forward_udp would/did transmit. */
};

/* Carried through every stage in the chain unchanged, alongside whatever
 * that stage's own process() actually transforms. capture_tsc in
 * particular has no meaning to most stages individually but is real
 * signal a much later consumer (Phase 2's epoch barrier, forward_udp's
 * optional --preserve-timing pacing, a future diagnostic stage) needs -
 * simplest to just always carry it rather than have every stage type
 * decide whether it cares. */
/* First bit of struct stage_record.flags below - a stage sets this on
 * its output record to signal "I judged this record bad" (a failed
 * header check, an out-of-range engineering value, a bad checksum,
 * etc.) without needing a dedicated port_type of its own - see this
 * enum's own header comment for why validity is deliberately NOT
 * encoded as a type. Purely opt-in and additive: a stage indifferent to
 * this bit can leave it untouched - see struct stage's own process()
 * comment below for the caller-side zero-init guarantee that makes that
 * safe. Since version 5, pipeline.c's dispatch loop actively acts on
 * this bit after every successful process() call - see graph_config.c's
 * per-node "on_invalid" ("drop"/"pass") and the optional dedicated
 * invalid-record edge it wires, which together decide what happens to a
 * flagged record without the stage itself needing to know or care.
 * validate_stage.c is the first stage to set it. One bit for now - grow
 * the bitmask later if a second concrete need (e.g. which specific
 * check failed) actually shows up, rather than reserving ranges
 * speculatively. */
#define STAGE_RECORD_FLAG_INTEGRITY_FAILED (1u << 0)

struct stage_record {
	enum stage_port_type type;
	uint8_t *data;    /* plain heap/stack memory owned by the pipeline
			    * runner, NOT an rte_mbuf - see this file's own
			    * header comment. Points into a per-record scratch
			    * buffer sized generously enough for any stage's
			    * output (see pipeline.h's STAGE_SCRATCH_BYTES). */
	uint32_t len;
	uint64_t capture_tsc;
	uint32_t flags;   /* Bitmask, see STAGE_RECORD_FLAG_* above. */
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
	unsigned out_port;       /* Which of the stage's declared output ports
				    * (see struct stage.out_port_count below)
				    * `out` targets; meaningless when ok=false.
				    * Selects which downstream node this record
				    * continues to - graph_config.c guarantees
				    * every value in [0, out_port_count()) is
				    * wired to a real edge, so any value in that
				    * range is always safe to return. Returning
				    * a value >= this stage instance's own
				    * declared count is a plugin bug: pipeline.c
				    * logs it distinctly and drops the record,
				    * rather than treating it as an ordinary
				    * drop. A single-output stage (the common
				    * case - see out_port_count's NULL default)
				    * just leaves this at its zero-init value. */
	const char *drop_reason; /* NULL if ok; always a string literal, never
				    * heap-allocated - this runs on the hot path
				    * once per record. */
};

/* One stage type's full contract. plugin_loader.c holds a dynamically-
 * populated table of these, one per successfully-loaded .so plugin
 * (see stage_abi.h) - every stage type this binary can run is known
 * once startup's plugin-loading pass finishes, before graph_config.c
 * ever runs, which is what still lets a JSON graph referencing an
 * unknown stage type fail cleanly at startup, exactly as it always
 * has: the set of available types is just discovered at process
 * startup now, not baked in at compile time. */
struct stage {
	const char *name;         /* matches a graph JSON node's "type" field
				     * exactly (see graph_config.c) */

	/* Bitmask of PORT_TYPE_BIT(...) values - every input type this
	 * stage's process() knows how to handle. Most stages accept exactly
	 * one (a single PORT_TYPE_BIT(x)); a stage like forward_udp that
	 * treats several types identically (verbatim bytes, regardless of
	 * which specific byte-blob type they came from) ORs them together.
	 * graph_config.c rejects wiring an edge whose upstream out_type
	 * isn't one of these bits. Since in->type on the hot path is then
	 * only guaranteed to be *one of* these, not always the same fixed
	 * value, a stage accepting more than one type that need genuinely
	 * different handling must itself branch on in->type inside
	 * process() to know which shape it actually received -
	 * dump_binary_stage.c accepts raw_record and wire_frame but treats
	 * them identically (verbatim bytes either way), so it happens not
	 * to need such a branch; a stage combining shapes that truly differ
	 * would. */
	unsigned in_types;
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

	/* How many output ports THIS NODE INSTANCE has - NULL means "always
	 * exactly 1," the right default for every stage shipped with this
	 * project except forward_udp (a genuine leaf - see below). Called
	 * once per graph node, immediately after that node's own init()
	 * succeeds above and before that node's edges are validated
	 * (deliberately an instance-level, config-dependent count - e.g. a
	 * routing-table stage's port count depends on how many entries are
	 * in that node's own config - not a fixed per-plugin-type constant);
	 * graph_config.c caches the result and never calls this again on the
	 * hot path. Returning a value > STAGE_MAX_OUT_PORTS is a startup
	 * error. Returning 0 declares this node a genuine leaf:
	 * graph_config.c then requires it to have zero outgoing edges - its
	 * out_type is unused in that case (nothing ever reads a leaf's
	 * *out; pipeline_run() returns as soon as process() succeeds,
	 * before ever looking at it), so a leaf can declare whatever
	 * out_type is most descriptive of what it actually does (e.g.
	 * dump_binary_stage.c dumps to a file, not the wire, but still
	 * needs to put something in this field). Otherwise, every port in
	 * [0, out_port_count()) must have
	 * exactly one outgoing edge in the graph - a stage declaring N ports
	 * but only wiring some of them is a startup error, not a silently
	 * unreachable port. Every output port of one node instance shares
	 * that instance's single out_type - there's no per-port output
	 * type. */
	unsigned (*out_port_count)(void *state);

	/* The hot-path function, called once per record that reaches this
	 * stage. in->type is guaranteed to be one of this stage's declared
	 * in_types (graph validation guarantees that by construction), but
	 * NOT always the same fixed value if in_types has more than one bit
	 * set - see in_types' own comment above. On ok=true, the caller
	 * expects *out to be fully populated
	 * (type=this stage's out_type, data/len/capture_tsc set) and passes
	 * it on to the next stage; on ok=false, *out is ignored.
	 *
	 * *out is zero-initialized by the caller before every call
	 * (pipeline.c's own comment on this marks it a deliberate,
	 * ABI-level guarantee, not incidental) - so out->flags in
	 * particular already reads 0 on entry, and a stage indifferent to
	 * STAGE_RECORD_FLAG_* need not touch it at all. A stage that DOES
	 * care must still set it explicitly, same as type/len/capture_tsc:
	 * "zeroed by default" is not "cleared for you before every
	 * intermediate write," and a stage must not assume anything about
	 * *out's contents beyond that initial zero state. */
	struct stage_result (*process)(void *state, const struct stage_record *in,
					struct stage_record *out);

	/* Mirror of init() - called once per stage instance at pipeline
	 * shutdown. NULL state (init() returned NULL for some reason it
	 * treated as non-fatal, or this stage type has no state at all) must
	 * be handled gracefully - not every stage needs teardown, but every
	 * stage struct has the pointer for uniformity. */
	void (*teardown)(void *state);

	/* Optional - NULL (the right default for most stages) means "this
	 * stage has nothing to report." Called periodically - every
	 * --status-poll-interval (default 2s, see main.c), NEVER per-record
	 * - from the process's MAIN lcore only, never a worker lcore, and
	 * never anywhere on the hot path (see pipeline.c/pipeline_worker.c,
	 * neither of which calls this). Fills *out (the caller
	 * zero-initializes it first, same ABI-level guarantee process()'s
	 * own *out gets) with whatever named counters this instance wants
	 * to expose right now - see struct stage_status above.
	 *
	 * A single node instance's state is already shared and concurrently
	 * WRITTEN by every worker lcore that ever routes a record to it
	 * (this is not new - see this struct's own top comment, and
	 * forward_udp_stage.c's shared-socket precedent for the same
	 * concurrency shape) - this callback then READS that same state
	 * from a completely different thread (the main lcore). Any counter
	 * exposed here needs to be updated with atomics in process()
	 * (atomic_fetch_add_explicit(..., memory_order_relaxed) is the
	 * existing convention - see pipeline_counters in pipeline.h) and
	 * read the same way here (atomic_load_explicit). Must not block -
	 * this runs on the same thread that also drives the status-server
	 * update loop and the shutdown poll. */
	void (*get_status)(void *state, struct stage_status *out);
};

#endif
