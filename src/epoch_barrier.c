#include <sched.h>
#include "epoch_barrier.h"

/* seq_cst throughout: enter()/exit() are a single atomic RMW each and
 * wait_if_pending()'s fast path is a single load, so the cost is
 * negligible on this project's scale - and it removes a whole class of
 * subtle cross-thread ordering bugs from code whose only job is
 * coordination. Revisit only if profiling says so (see epoch_barrier.h). */

void
epoch_barrier_init(struct epoch_barrier *eb)
{
	atomic_store(&eb->current_epoch, 0);
	atomic_store(&eb->in_flight, 0);
	atomic_store(&eb->barrier_pending, false);
}

void
epoch_barrier_wait_if_pending(struct epoch_barrier *eb)
{
	while (atomic_load(&eb->barrier_pending))
		sched_yield();
}

uint64_t
epoch_barrier_enter(struct epoch_barrier *eb)
{
	atomic_fetch_add(&eb->in_flight, 1);
	return atomic_load(&eb->current_epoch);
}

void
epoch_barrier_exit(struct epoch_barrier *eb)
{
	atomic_fetch_sub(&eb->in_flight, 1);
}

uint64_t
epoch_barrier_drain(struct epoch_barrier *eb, uint64_t barrier_seq, uint64_t barrier_tsc)
{
	(void)barrier_seq;
	(void)barrier_tsc;

	atomic_store(&eb->barrier_pending, true);
	while (atomic_load(&eb->in_flight) != 0)
		sched_yield();

	uint64_t new_epoch = atomic_fetch_add(&eb->current_epoch, 1) + 1;

	atomic_store(&eb->barrier_pending, false);
	return new_epoch;
}

uint64_t
epoch_barrier_current_epoch(struct epoch_barrier *eb)
{
	return atomic_load(&eb->current_epoch);
}
