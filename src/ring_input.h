#ifndef RING_INPUT_H
#define RING_INPUT_H

#include <rte_ring.h>

/* Creates the input rte_ring (SP/SC in v1 - testgen.c's pthread is the
 * sole producer, the pipeline's main lcore the sole consumer). Phase 4
 * replaces this with rte_ring_lookup() against a ring a separate
 * chrontabulator process created (DPDK multi-process, secondary
 * process attaching to primary-owned shared memory) instead of creating
 * one locally - kept as this one choke-point function so that change
 * touches a single call site, not every caller. Returns NULL on failure
 * (check rte_errno, same convention as every other rte_ring_create()
 * caller). */
struct rte_ring *ring_input_create(const char *name, unsigned int size);

#endif
