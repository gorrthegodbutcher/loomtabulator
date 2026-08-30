#ifndef EPOCH_BARRIER_H
#define EPOCH_BARRIER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Cross-worker epoch/watermark barrier - see the project plan's "Phase 2"
 * section (~/.claude/plans/noble-kindling-lemon.md) for the full design
 * rationale. Deliberately DPDK-free (<stdatomic.h> only) so it links into
 * a plain host test binary exactly like tests/test_stage_chain.c does -
 * see tests/test_epoch_barrier.c.
 *
 * The problem: barrier records (record.h's CHRONO_BARRIER_MAGIC) mark
 * epoch boundaries in the input ring. Data records within an epoch may be
 * processed out of order across worker cores, but every data record
 * belonging to epoch N must finish processing before any data record
 * belonging to epoch N+1 is considered to belong to N+1 - "data between
 * sequential timestamps must all be processed and forwarded before the
 * second timestamp" (the user's own framing).
 *
 * The mechanism: each worker calls epoch_barrier_wait_if_pending() before
 * every dequeue, epoch_barrier_enter()/epoch_barrier_exit() around every
 * data record it processes, and epoch_barrier_drain() when it dequeues a
 * barrier record itself. drain() sets barrier_pending (parking every
 * other worker at their next wait_if_pending() call - this is what stops
 * another worker from racing ahead and mis-tagging a new record with the
 * stale epoch), waits for in_flight to reach zero (every data record
 * dequeued before this barrier - guaranteed by the ring's own
 * FIFO-across-consumers ordering to include all of the current epoch's
 * records - has now finished processing), advances current_epoch, then
 * clears barrier_pending to release everyone. This makes each epoch
 * transition a genuine stop-the-world moment for the worker pool - a real
 * but small throughput cost, and exactly what the ordering requirement
 * above asks for, not over-engineering.
 *
 * Correctness note: there is one narrow boundary case worth documenting
 * rather than silently trusting. A worker A can pass
 * wait_if_pending() (observing barrier_pending == false) and then be
 * preempted before its own enter() call runs, while another worker B
 * concurrently dequeues the barrier, calls drain(), observes
 * in_flight == 0, advances current_epoch, and clears barrier_pending -
 * all before A's enter() actually executes. The theoretically bad case
 * requires drain()'s in_flight==0 check and the epoch advance to both
 * complete within the handful of nanoseconds between A's own atomic load
 * and A's own atomic increment, with no intervening scheduling -
 * vanishingly unlikely on real hardware, not proven impossible with this
 * 3-atomics design alone. Ship the simple version; validate hard with
 * tests/test_epoch_barrier.c (including a -fsanitize=thread run and a
 * high-iteration-count repeat, since this class of bug is
 * timing-dependent). If stress testing ever surfaces it in practice, the
 * standard fix is a generation-counter grace-period wait (each worker
 * tracks a per-loop-iteration counter; drain() waits for every worker's
 * counter to have advanced past its value at dequeue time, on top of
 * in_flight == 0) - the same technique RCU-style reclamation uses. Treat
 * this exactly like rte_reorder in the wider project plan: a known,
 * understood fallback, not something to build without evidence it's
 * needed.
 *
 * Update: that stress testing was actually run (while prototyping an
 * unrelated Phase 3 hot-swap feature that would have added a second,
 * similar flag alongside barrier_pending) and DID surface this race in
 * practice - about 1% of plain runs and ~12% of -fsanitize=thread runs
 * hit it. So "vanishingly unlikely" is now a measured ~1%, not a
 * theoretical bound. Three fix attempts (the generation-counter grace
 * period this comment names, an announce-before-check reordering, and
 * producer-side epoch tagging) were tried; each closed the specific
 * failure just found and stress-testing then found a different one -
 * see CLAUDE.md's "Phase 3 design sketch" section for the fuller trace.
 * None of those attempts are in this file - this is still the original,
 * simple 3-atomics version, unfixed. Treat the ~1% number as current
 * and real, and budget real effort (or a different approach entirely -
 * a lock around the dequeue+enter sequence, or per-record epoch tags
 * from the producer, are the two most promising directions tried so
 * far) if this is ever revisited, not another quick attempt.
 *
 * The drain wait is provably bounded and can't deadlock: stage.h's own
 * contract guarantees every process() call terminates with a definite
 * ok/drop result (no stage blocks indefinitely), so every in_flight
 * increment is guaranteed a matching decrement in finite time, and no new
 * increments occur once a worker observes barrier_pending true at its own
 * wait_if_pending() call. */
struct epoch_barrier {
	_Atomic uint64_t current_epoch;
	_Atomic uint64_t in_flight;
	_Atomic bool     barrier_pending;
};

void epoch_barrier_init(struct epoch_barrier *eb);

/* Call at the top of every worker-loop iteration, before attempting to
 * dequeue anything. Parks (spin/poll, no lock) while another worker is
 * inside epoch_barrier_drain(). Must be checked before dequeuing, not
 * just before processing - see this file's header comment for why that
 * ordering is load-bearing. */
void epoch_barrier_wait_if_pending(struct epoch_barrier *eb);

/* Call immediately before running a just-dequeued DATA record through
 * the stage chain. Increments in_flight and returns the epoch this
 * record is considered part of (current_epoch at the moment of the
 * call) - callers may ignore the return value today (no epoch-aware
 * output hook exists yet), but it's threaded through now so a future one
 * doesn't need an API change. */
uint64_t epoch_barrier_enter(struct epoch_barrier *eb);

/* Call after processing finishes for a record epoch_barrier_enter() was
 * called for - regardless of the stage chain's ok/drop result.
 * Decrements in_flight. */
void epoch_barrier_exit(struct epoch_barrier *eb);

/* Call when a just-dequeued ring item is a BARRIER record instead of
 * data. Blocks until every data record dequeued ahead of this barrier
 * has finished processing, then advances current_epoch. barrier_seq/
 * barrier_tsc are the barrier record's own hdr->seq/hdr->capture_tsc -
 * passed through only for logging/cross-checking (see record.h), not
 * required by the algorithm itself. Returns the new current_epoch. */
uint64_t epoch_barrier_drain(struct epoch_barrier *eb, uint64_t barrier_seq, uint64_t barrier_tsc);

/* Status/logging only - never used as part of the synchronization
 * protocol itself (that's entirely enter/exit/drain/wait_if_pending).
 * Takes a non-const pointer, like every other function here, since
 * atomic_load() on a const-qualified _Atomic object is awkward to spell
 * portably - this is a plain read, not a mutation. */
uint64_t epoch_barrier_current_epoch(struct epoch_barrier *eb);

#endif
