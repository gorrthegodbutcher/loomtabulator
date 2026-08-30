#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include "testgen.h"
#include "record.h"

static volatile bool g_stop_requested;

static void
put_be64(uint8_t *p, uint64_t v)
{
	for (int i = 7; i >= 0; i--) {
		p[i] = (uint8_t)v;
		v >>= 8;
	}
}

static void
emit_barrier(struct rte_ring *ring, uint64_t epoch_id)
{
	/* rte_malloc(), not malloc() - see testgen_run()'s own comment on
	 * why every ring item, barrier or data, uses the same allocator
	 * pipeline_worker.c's consumer side frees with. */
	uint8_t *blob = rte_malloc(NULL, sizeof(struct chrono_record_hdr), 0);
	if (blob == NULL)
		return; /* transient - the next barrier attempt will retry */

	struct chrono_record_hdr *hdr = (struct chrono_record_hdr *)blob;
	hdr->magic = CHRONO_BARRIER_MAGIC;
	hdr->seq = epoch_id;
	hdr->capture_tsc = rte_rdtsc();
	hdr->len = 0;
	hdr->reserved = 0;

	if (rte_ring_enqueue(ring, blob) != 0)
		rte_free(blob);
}

void *
testgen_run(void *arg)
{
	struct testgen_config *cfg = arg;
	uint64_t sent = 0;
	uint64_t since_last_barrier = 0;
	uint64_t epoch_id = 0;
	uint64_t interval_us = cfg->rate_per_sec != 0 ? 1000000ULL / cfg->rate_per_sec : 0;

	while (!g_stop_requested && (cfg->count == 0 || sent < cfg->count)) {
		/* rte_malloc(), not malloc() - a ring item's producer isn't
		 * always this in-process thread (see ring_input.h's own
		 * Phase 4 comment: a DPDK secondary process can enqueue
		 * directly too), so every producer needs to agree on one
		 * allocator with pipeline_worker.c's consumer side, which
		 * frees every dequeued item unconditionally. rte_malloc()'s
		 * hugepage-backed memory is also what makes a pointer placed
		 * on the ring meaningful to a *different* process in the
		 * first place - plain malloc() memory is private to whichever
		 * process allocated it. */
		size_t total = sizeof(struct chrono_record_hdr) + cfg->payload_len;
		uint8_t *blob = rte_malloc(NULL, total, 0);
		if (blob == NULL) {
			/* Transient allocation pressure - back off briefly and
			 * retry rather than treat this as fatal; testgen is a
			 * test tool, not the real datapath. */
			usleep(1000);
			continue;
		}

		struct chrono_record_hdr *hdr = (struct chrono_record_hdr *)blob;
		hdr->magic = CHRONO_RECORD_MAGIC;
		hdr->seq = sent;
		hdr->capture_tsc = rte_rdtsc();
		hdr->len = cfg->payload_len;
		hdr->reserved = 0;

		/* Payload: an 8-byte big-endian "raw sensor" counter at offset
		 * 0, incrementing once per record, zero-padded after that -
		 * matches the v1 example graph's extract stage config
		 * (field_offset_bytes=0, field_width_bytes=8), and gives
		 * dpdk-app-example --receiver's sequence tracking something
		 * real to verify against on the far end (see the plan's
		 * verification step 4). */
		uint8_t *payload = blob + sizeof(*hdr);
		memset(payload, 0, cfg->payload_len);
		put_be64(payload, sent);

		if (rte_ring_enqueue(cfg->ring, blob) != 0)
			rte_free(blob); /* ring full - drop, same as any other
					   * backpressure-drop in this project family */

		sent++;
		since_last_barrier++;
		if (cfg->barrier_every != 0 && since_last_barrier >= cfg->barrier_every) {
			emit_barrier(cfg->ring, epoch_id);
			epoch_id++;
			since_last_barrier = 0;
		}

		if (interval_us != 0)
			usleep(interval_us);
	}

	return NULL;
}

void
testgen_stop(void)
{
	g_stop_requested = true;
}
