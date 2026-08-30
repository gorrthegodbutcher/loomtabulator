#ifndef STAGE_ABI_H
#define STAGE_ABI_H

#include <stdint.h>
#include "stage.h"

/* The plugin ABI contract - see src/plugin_loader.c's header comment
 * for the full loading protocol. A plugin is a .so exporting exactly
 * two module-level functions, called once each at load time, in this
 * order:
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
 *       in_type/out_type, and its init/out_port_count/process/teardown
 *       function pointers (see stage.h). NULL is a valid "reject me"
 *       response.
 *
 * These two exports are NOT the whole lifecycle - struct stage's own
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
 * Both exports are FUNCTIONS, not data symbols - deliberately, so a
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
 * Version 3 (current): replaced struct stage.max_out_ports (a static
 * per-plugin-type ceiling) with struct stage.out_port_count(state), a
 * dynamic per-instance callback - Option 2 from
 * docs/RECOMMENDATIONS.md's own §3.2, chosen because it lets
 * graph_config.c validate a node's wiring exactly ("declared 3 ports,
 * wired 2" is now a startup error, not just "index in range"), at the
 * cost of needing that node's own init() to run before its edges are
 * validated - graph_config.c already does that. Multi-port routing is
 * now fully executable: graph_config.c builds a real tree instead of a
 * flat chain, and pipeline.c walks it by reading struct
 * stage_result.out_port at each step (see stage.h and pipeline.h). */
#define STAGE_ABI_VERSION 3u

#define STAGE_ABI_VERSION_SYMBOL "loom_stage_abi_version"
#define STAGE_ABI_ENTRY_SYMBOL   "loom_stage_entry"

typedef uint32_t (*loom_stage_abi_version_fn)(void);
typedef const struct stage *(*loom_stage_entry_fn)(void);

#endif
