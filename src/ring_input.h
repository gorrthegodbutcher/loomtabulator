#ifndef RING_INPUT_H
#define RING_INPUT_H

#include <rte_ring.h>

/* Creates the input rte_ring - single-producer/multi-consumer as of
 * Phase 2 (testgen.c's pthread remains the sole producer, but N pipeline
 * worker lcores now dequeue competitively from it - see
 * pipeline_worker.c and epoch_barrier.h for the ordering guarantee this
 * depends on: rte_ring preserves overall FIFO order across all consumers
 * combined). Phase 4 replaces this with rte_ring_lookup() against a ring
 * a separate chrontabulator process created (DPDK multi-process,
 * secondary process attaching to primary-owned shared memory) instead
 * of creating one locally - kept as this one choke-point function so
 * that change touches a single call site, not every caller. Returns
 * NULL on failure (check rte_errno, same convention as every other
 * rte_ring_create() caller).
 *
 * Whoever enqueues onto this ring - testgen.c today, any external
 * secondary process attaching to it (already possible, confirmed
 * working - see loomlets/gfp_test_driver, a sibling project) once
 * Phase 4 replaces testgen with a real one - MUST allocate every blob
 * with rte_malloc(), never plain malloc(). pipeline_worker.c's
 * consumer side calls rte_free() unconditionally on every dequeued
 * item, and only DPDK's own hugepage-backed allocator produces memory
 * a *different* process can safely dereference or free at all -
 * ordinary heap memory is private to whichever process allocated it,
 * and mixing allocators (rte_malloc + free(), or malloc() + rte_free())
 * corrupts both heaps' bookkeeping, not just the one call that's
 * technically wrong. */
struct rte_ring *ring_input_create(const char *name, unsigned int size);

#endif
