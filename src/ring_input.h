#ifndef RING_INPUT_H
#define RING_INPUT_H

#include <stdbool.h>
#include <rte_ring.h>

/* Finds or creates the input rte_ring - rte_ring_lookup() first (in
 * case a ring under this exact name already exists in this instance's
 * --file-prefix namespace, created by a DIFFERENT, upstream process's
 * ring_output stage - see ring_output_stage.c and docs/MANAGEMENT.md's
 * Part 2), falling back to rte_ring_create() otherwise - the ordinary
 * single-process case (testgen.c is the sole producer today; Phase 4
 * makes a real chrontabulator process the producer instead, attaching
 * as a DPDK secondary the same way an upstream ring_output's downstream
 * consumer does). Single-producer/multi-consumer when THIS function
 * creates it (RING_F_SP_ENQ) - N pipeline worker lcores dequeue
 * competitively either way (see pipeline_worker.c and epoch_barrier.h
 * for the ordering guarantee that depends on: rte_ring preserves
 * overall FIFO order across all consumers combined) - but a ring found
 * via lookup keeps whatever flags ITS creator gave it (ring_output's
 * own ring is created multi-producer, since a daisy chain's upstream
 * process can have more than one worker lcore reaching that node
 * concurrently); lookup returns the same ring object regardless, so
 * this function's own dequeue-side callers don't need to know or care
 * which case they're in. Kept as this one choke-point function so
 * either path touches a single call site, not every caller. Returns
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
 * technically wrong.
 *
 * A DPDK secondary process finds this ring (and the primary's memory
 * layout generally) by matching file-prefix - main.c now defaults to
 * --file-prefix=loomtabulator (see main.c's build_eal_argv() and
 * README.md's "Usage" section) rather than DPDK's own "rte" default,
 * so a secondary process's own invocation needs a matching
 * --file-prefix=loomtabulator too, or it won't find this ring at all.
 * A primary launched with its own explicit --file-prefix override
 * needs secondaries to match THAT value instead, obviously.
 *
 * *out_owns_ring is set to true if THIS call is what created the ring
 * (rte_ring_create() path) and false if it merely attached to one that
 * already existed (rte_ring_lookup() path) - same owns_ring shape
 * ring_output_stage.c already uses for the exact same reason. The
 * caller (main.c) must only ever call rte_ring_free() on this ring, or
 * drain it via rte_ring_dequeue() at shutdown, when this is true -
 * doing either to a ring this process merely attached to would destroy
 * or steal from a ring an upstream ring_output-owning process still
 * depends on, the double-free/data-loss risk this split exists to
 * prevent. */
struct rte_ring *ring_input_create(const char *name, unsigned int size, bool *out_owns_ring);

#endif
