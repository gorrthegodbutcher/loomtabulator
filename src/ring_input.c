#include <rte_ring.h>
#include <rte_eal.h>
#include "ring_input.h"

struct rte_ring *
ring_input_create(const char *name, unsigned int size)
{
	return rte_ring_create(name, size, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
}
