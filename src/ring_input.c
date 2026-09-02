#include <rte_ring.h>
#include <rte_eal.h>
#include "ring_input.h"

struct rte_ring *
ring_input_create(const char *name, unsigned int size, bool *out_owns_ring)
{
	/* A ring_output stage in a DIFFERENT, upstream loomtabulator
	 * instance (same --file-prefix namespace - see
	 * ring_output_stage.c) may have already created a ring under this
	 * exact name, if this instance is a downstream hop in a daisy
	 * chain (docs/MANAGEMENT.md's Part 2) - rte_ring_lookup() finds
	 * that existing ring instead of erroring out trying to recreate
	 * it. This is exactly the change ring_input.h's own header comment
	 * already anticipated for Phase 4's chrontabulator integration -
	 * the daisy-chain case just needed it first. Falls through to the
	 * ordinary rte_ring_create() below when lookup finds nothing - the
	 * single-process/testgen case, or simply being the first instance
	 * up in a chain that hasn't started producing yet. */
	struct rte_ring *existing = rte_ring_lookup(name);
	if (existing != NULL) {
		*out_owns_ring = false;
		return existing;
	}

	/* Phase 2: multi-consumer dequeue - N worker lcores now pull
	 * competitively from this ring (see epoch_barrier.h for the ordering
	 * guarantee this relies on). Still single-producer: testgen.c today,
	 * chrontabulator in Phase 4, or an upstream ring_output stage in a
	 * daisy chain - RING_F_SP_ENQ assumes whichever of those it is, it's
	 * only ever exactly one producer for THIS ring. ring_output_stage.c
	 * creates its OWN ring with no such flag, since its own producer
	 * side (this process's worker lcores) is genuinely multi-producer -
	 * the flag choice is per-ring, made by whichever side calls
	 * rte_ring_create() for it, not a property this function enforces
	 * on every ring anywhere. */
	struct rte_ring *created = rte_ring_create(name, size, rte_socket_id(), RING_F_SP_ENQ);
	if (created != NULL)
		*out_owns_ring = true;
	return created;
}
