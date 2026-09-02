#ifndef STAGE_ABI_H
#define STAGE_ABI_H

#include <stdint.h>
#include "stage.h"

/* The plugin ABI contract - see src/plugin_loader.c's header comment
 * for the full loading protocol. An ordinary, single-stage plugin is a
 * .so exporting exactly two module-level functions, called once each at
 * load time, in this order:
 *
 *   uint32_t loom_stage_abi_version(void);
 *       Must return STAGE_ABI_VERSION below. Checked BEFORE anything
 *       else is touched from the handle - a mismatch closes the
 *       handle immediately and skips the plugin (a clean, logged
 *       startup-time rejection, never a runtime crash from a stale
 *       struct layout).
 *
 *   const struct stage *loom_stage_entry(void);
 *       Returns a pointer to a single, static (not stack/heap)
 *       struct stage describing this plugin's one stage type - name,
 *       in_types/out_type, and its init/out_port_count/process/teardown
 *       function pointers (see stage.h). NULL is a valid "reject me"
 *       response.
 *
 * A "loomlet" - this project's own term for a .so bundling more than
 * one stage type together (a family of related stages sharing common
 * code, built and shipped as one plugin instead of one .so per stage
 * type) - exports a THIRD, mutually-exclusive-with-loom_stage_entry
 * function instead:
 *
 *   const struct stage *loom_stage_entry_at(unsigned index);
 *       Called with index = 0, 1, 2, ... in order until it returns
 *       NULL, which ends the sequence (a plugin returning NULL for
 *       index 0 declares itself a loomlet with zero stages - logged
 *       and skipped, not fatal, same posture an ordinary plugin's own
 *       NULL loom_stage_entry() already has). Each non-NULL return is
 *       registered exactly like a single-stage plugin's one stage
 *       would be - same per-stage name-collision and registry-size
 *       checks, same log line per stage. plugin_loader.c checks (via
 *       dlsym()) whether a .so exports loom_stage_entry_at BEFORE ever
 *       looking for loom_stage_entry - an ordinary single-stage plugin
 *       simply doesn't export this symbol at all, so it takes the
 *       original single-stage path completely unchanged; a loomlet
 *       does not need to ALSO export loom_stage_entry - implement
 *       exactly one of the two, never both.
 *
 * Deliberately NOT a STAGE_ABI_VERSION bump: this extends the loading
 * PROTOCOL (which two/three symbols a .so exports and how they're
 * called), not the layout or meaning of struct stage/stage_record/
 * stage_result themselves - the version-bump rule below is specifically
 * about those three structs. An existing single-stage plugin needs no
 * changes and no rebuild for this - it already works today, unaffected.
 *
 * These exports are NOT the whole lifecycle - struct stage's own
 * init(config)/teardown(state) are the real per-instance hooks, called
 * once per GRAPH NODE that uses this stage type (not once per .so),
 * before/after that node ever processes data. A stage needing to set
 * up several resources from one node's config (multiple destinations,
 * for example) does it inside its own init()/teardown() - see
 * forward_udp_stage.c for the existing pattern (one socket, opened in
 * init(), closed in teardown()). There is deliberately no third,
 * module-level init/shutdown pair - every built-in stage's state is
 * already fully expressible per-node, and adding a second lifecycle
 * tier this project doesn't need yet would just be surface area to get
 * wrong.
 *
 * All exports are FUNCTIONS, not data symbols - deliberately, so a
 * plugin's own build flags (LTO, dead-symbol stripping) can never
 * accidentally drop an unreferenced-looking `const` global before
 * dlsym() ever gets to look for it.
 *
 * ABI stability rule: bump STAGE_ABI_VERSION on ANY change to the
 * layout or meaning of struct stage, struct stage_record, or struct
 * stage_result (stage.h) - even a purely additive field. The loader
 * does a hard equality check, never a >=/compatibility-window check,
 * because there is no reserved padding or versioned-struct scheme
 * here. See plugin-sdk/README.md for the full set of build rules a
 * third-party plugin author needs to follow (no bitfields, no
 * #pragma pack, build for the host's own target triple).
 *
 * Version 2: added struct stage.max_out_ports, struct
 * stage_result.out_port, and struct stage_record.flags (see stage.h) -
 * groundwork for a stage declaring more than one output port and for a
 * stage flagging a record's integrity as suspect, requested by
 * docs/RECOMMENDATIONS.md. max_out_ports > 1 was accepted by the loader
 * but rejected by graph_config.c at graph-load time - the pipeline
 * engine only executed single-output chains at that point.
 *
 * Version 3: replaced struct stage.max_out_ports (a static
 * per-plugin-type ceiling) with struct stage.out_port_count(state), a
 * dynamic per-instance callback - Option 2 from
 * docs/RECOMMENDATIONS.md's own §3.2, chosen because it lets
 * graph_config.c validate a node's wiring exactly ("declared 3 ports,
 * wired 2" is now a startup error, not just "index in range"), at the
 * cost of needing that node's own init() to run before its edges are
 * validated - graph_config.c already does that. Multi-port routing is
 * now fully executable: graph_config.c builds a real tree instead of a
 * flat chain, and pipeline.c walks it by reading struct
 * stage_result.out_port at each step (see stage.h and pipeline.h).
 *
 * Version 4: replaced struct stage.in_type (a single enum
 * stage_port_type) with struct stage.in_types, a PORT_TYPE_BIT(...)
 * bitmask - a stage can now legitimately accept more than one input
 * type (e.g. forward_udp accepted raw_record/validated/extracted alike,
 * since a UDP payload is just bytes regardless of which of those three
 * produced it). graph_config.c's edge check became a membership test
 * instead of an equality test. Also dropped the requirement that a
 * leaf (out_port_count() == 0) must produce PORT_TYPE_WIRE_FRAME - that
 * rule was only ever a stand-in for "ends in forward_udp," which
 * stopped being the only kind of terminal sink once dump_binary/
 * dump_text (both leaves, neither producing a wire frame) were added;
 * a leaf's out_type is unused and unconstrained now.
 *
 * Version 5 (current): enum stage_port_type shrank from 5 values to 3 -
 * PORT_TYPE_VALIDATED and PORT_TYPE_EXTRACTED are gone. Both only ever
 * described the SAME wire shape as PORT_TYPE_RAW_RECORD (an opaque byte
 * blob), differing only in "has this been checked" or "has this been
 * narrowed to a sub-slice" - neither is a real shape difference, and
 * extract's own byte-slice mode (added just before this version) proved
 * the model actively harmful: it made PORT_TYPE_EXTRACTED describe two
 * incompatible things (a canonical 8-byte value, or an arbitrary-width
 * slice) depending on config, defeating the entire point of a small,
 * checkable type set. Every stage's in_types/out_type that referenced
 * either removed value now uses PORT_TYPE_RAW_RECORD instead - validate
 * and extract (both modes) are now simple PORT_TYPE_RAW_RECORD ->
 * PORT_TYPE_RAW_RECORD stages, and convert/forward_udp/dump_binary's
 * in_types collapse accordingly (see each stage's own plugin shim).
 * "Has this record passed a check" moved to the ALREADY-existing
 * struct stage_record.flags (STAGE_RECORD_FLAG_INTEGRITY_FAILED, see
 * stage.h) instead - an attribute of the data, not a type of the data,
 * and one that stacks (a future range-check stage can flag independent
 * of whatever validate already flagged). pipeline.c's dispatch loop
 * gained real behavior keyed on that flag - see graph_config.c's new
 * per-node "on_invalid" ("drop"/"pass") and optional dedicated
 * invalid-record edge - so ANY stage, built-in or third-party, gets
 * flagged-record routing for free just by setting the bit, without
 * declaring extra output ports or writing any routing logic itself.
 * This is an engine-internal addition (struct pipeline_stage_instance,
 * pipeline.h) - it does NOT touch struct stage/stage_record/
 * stage_result's layout, so it isn't independently why this is a new
 * ABI version; the enum shrink is.
 *
 * Version 6 (current): added struct stage.get_status(state, out) - an
 * optional callback (NULL, the default, means "nothing to report") a
 * stage instance uses to expose its own named counters (e.g.
 * "records_checked", "records_flagged") for the web UI to display,
 * polled at a slow (--status-poll-interval, default 2s - not a
 * hot-path concern) cadence from main.c's own status loop, never from
 * a worker lcore. See struct stage_status/struct stage_status_field
 * (stage.h) for the fixed-size, pointer-free shape this reports
 * through - deliberately not a returned string (no lifetime/ownership
 * question crossing the ABI boundary) and deliberately counters-only
 * for now (uint64), not a richer typed value - every example this was
 * designed against is a counter, and widening later is a small,
 * additive change if a real need shows up. Since a node instance's
 * state is already concurrently written by every worker lcore that
 * routes a record to it, any stage implementing this needs atomics for
 * whatever it exposes - see get_status's own comment in stage.h. Every
 * existing stage (built-in or third-party) simply leaves this NULL by
 * default via C's own designated-initializer zero-fill, so this bump
 * needs a rebuild but no source change unless a stage author actually
 * wants to opt in.
 *
 * Version 7 (current): added struct stage.get_config_schema(out) - an
 * optional callback (NULL default, same zero-fill rollout story version
 * 6 already established) a stage TYPE uses to describe its own config
 * fields (name, type, required, numeric range, enum values, default,
 * one level of array-of-object nesting, and simple same-object
 * conditional fields via depends_on_field/depends_on_value) so the web
 * UI can render a real form instead of a raw JSON textarea - see
 * struct stage_config_field/struct stage_config_schema (stage.h) for
 * the fixed-size, pointer-free shape this reports through, same
 * lifetime discipline struct stage_status already established for the
 * exact same reason. Unlike get_status, this is called once per stage
 * TYPE with no instance and no init() involved (see plugin_loader.c's
 * registry and web_status.c's handle_stage_types()) - a schema
 * describes what a config CAN look like, not what one particular
 * node's config currently holds, so there's no state to build. This
 * does not add a second validation path: a stage's own init() (checked
 * at graph_config_load() time, same as always) remains the sole real
 * enforcement of a config value; the schema exists purely to build a
 * GUI form and pre-populate sensible inputs. extract_stage.c is the
 * worked example for depends_on_field (field_width_bytes only applies
 * when mode == "numeric", field_length_bytes only when mode ==
 * "bytes") - see its own get_config_schema for the exact shape a
 * conditional field takes. */
#define STAGE_ABI_VERSION 7u

#define STAGE_ABI_VERSION_SYMBOL "loom_stage_abi_version"
#define STAGE_ABI_ENTRY_SYMBOL   "loom_stage_entry"
/* Multi-stage "loomlet" export - see this file's own top comment for
 * the full protocol. Deliberately not part of the Version N history
 * above: it's a loading-protocol addition, not a change to struct
 * stage/stage_record/stage_result, so it doesn't bump
 * STAGE_ABI_VERSION and doesn't need one of these entries. */
#define STAGE_ABI_ENTRY_AT_SYMBOL "loom_stage_entry_at"

typedef uint32_t (*loom_stage_abi_version_fn)(void);
typedef const struct stage *(*loom_stage_entry_fn)(void);
typedef const struct stage *(*loom_stage_entry_at_fn)(unsigned index);

#endif
