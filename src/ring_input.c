#include <rte_ring.h>
#include <rte_eal.h>
#include "ring_input.h"

struct rte_ring *
ring_input_create(const char *name, unsigned int size)
{
	/* Phase 2: multi-consumer dequeue - N worker lcores now pull
	 * competitively from this ring (see epoch_barrier.h for the ordering
	 * guarantee this relies on). Still single-producer: testgen.c today,
	 * chrontabulator in Phase 4. */
	return rte_ring_create(name, size, rte_socket_id(), RING_F_SP_ENQ);
}
