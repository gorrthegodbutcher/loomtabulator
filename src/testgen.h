#ifndef TESTGEN_H
#define TESTGEN_H

#include <stdint.h>
#include <rte_ring.h>

/* Stands in for chrontabulator's not-yet-built replay feature (Phase 4)
 * - writes chrono_record_hdr-shaped records onto the input ring so v1's
 * pipeline can be built and verified end-to-end before that real
 * producer exists. Deliberately a plain pthread, not an EAL lcore - the
 * real future producer (chrontabulator, via DPDK multi-process) will be
 * a genuinely separate OS process too, not a thread in this one, so
 * keeping this a thread (rather than lcore-bound datapath code) matches
 * that shape and keeps main.c's own lcore free to just run the pipeline. */

struct testgen_config {
	struct rte_ring *ring;
	uint32_t rate_per_sec; /* 0 = as fast as possible */
	uint64_t count;         /* 0 = infinite (until testgen_stop()) */
	uint32_t payload_len;   /* each record's payload size in bytes - must
				  * be at least 8 so the v1 example graph's
				  * extract stage (field_offset_bytes=0,
				  * field_width_bytes=8) has something to read */
};

/* pthread entry point - pass a heap-allocated (or stack-stable for the
 * thread's lifetime) struct testgen_config* as arg. Runs until count
 * records have been sent (if count != 0) or testgen_stop() is called. */
void *testgen_run(void *arg);

/* Signals a running testgen_run() to stop after its current record -
 * safe to call from another thread (a single volatile flag, no lock
 * needed for a one-way stop signal). */
void testgen_stop(void);

#endif
