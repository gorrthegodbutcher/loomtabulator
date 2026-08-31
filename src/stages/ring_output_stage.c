#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <rte_ring.h>
#include <rte_malloc.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include "ring_output_stage.h"
#include "../record.h"

/* raw_record -> a named rte_ring, for a second loomtabulator instance's
 * own input ring to attach to (see ring_input.c's matching
 * lookup-before-create change) - the daisy-chaining mechanism specced
 * in docs/MANAGEMENT.md. A leaf, like forward_udp/dump_binary - nothing
 * downstream in THIS process's graph ever reads *out.
 *
 * Unlike every other built-in stage, this one is NOT DPDK-independent
 * (stage.h's own header comment - "no stage type touches DPDK/mbuf APIs
 * at all anymore" - stops being true here, deliberately: an rte_ring IS
 * the thing being produced). See this repo's Makefile for the build-rule
 * consequence (this plugin links against libdpdk, unlike every other
 * one) and plugin-sdk/README.md if this ever needs documenting for a
 * third-party plugin author - plugin-sdk/ itself still vendors no DPDK
 * headers, so a third party copying that convention for their own
 * ring-producing stage needs to add that dependency themselves.
 *
 * Re-synthesizes a fresh struct chrono_record_hdr around whatever bytes
 * this stage actually receives, rather than assuming in->data already
 * has one - an upstream extract (byte-slice mode) node feeding this one
 * has already narrowed data down to a bare payload sub-slice with no
 * header of its own (see extract_stage.c), so this is the only point
 * that can put one back. seq is this instance's own monotonic counter
 * (struct stage_record carries no seq field to pass through - only
 * capture_tsc, which IS carried through unchanged, same as every other
 * stage leaves it, since it's real signal a downstream consumer's own
 * epoch_barrier.c could eventually key off of). magic is always
 * CHRONO_RECORD_MAGIC - this stage never produces a barrier record
 * (barrier records don't flow through the stage chain at all - see
 * pipeline_worker.c/record.h).
 *
 * Created with NO rte_ring flags (full multi-producer/multi-consumer),
 * not ring_input_create()'s RING_F_SP_ENQ - deliberately: any of THIS
 * process's worker lcores can reach this node concurrently (which one
 * depends purely on which worker happened to dequeue a given record
 * from the input ring), so the enqueue side is genuinely
 * multi-producer, unlike testgen.c's single dedicated producer thread.
 * The consuming instance's own --workers=N can also be > 1, so the
 * dequeue side can't assume single-consumer either. */

struct ring_output_config {
	struct rte_ring *ring;

	/* This instance's own seq counter for records it produces - see
	 * this file's header comment. Relaxed atomics: ordering across
	 * concurrent producers doesn't matter, only that each gets a
	 * distinct value (fetch_add is inherently that). */
	atomic_uint_least64_t next_seq;

	/* Exposed via get_status() below - same concurrent-writer/
	 * slow-single-reader shape every other built-in's counters
	 * already have (see stage.h's own get_status comment). */
	atomic_uint_least64_t records_enqueued;
	atomic_uint_least64_t records_dropped;
	atomic_uint_least64_t bytes_enqueued;
};

void *
ring_output_stage_init(const struct json_value *config)
{
	const char *ring_name = json_as_string(json_object_get(config, "ring_name"), NULL);
	if (ring_name == NULL)
		return NULL;
	unsigned int ring_size = (unsigned int)json_as_number(json_object_get(config, "ring_size"), 4096);

	struct ring_output_config *st = calloc(1, sizeof(*st));
	if (st == NULL)
		return NULL;

	/* This side always creates (never looks up) - the downstream
	 * instance's own ring_input.c is the side that looks up by name,
	 * matching ring_input.h's existing "producer creates, consumer
	 * attaches" convention. A name collision here (two ring_output
	 * nodes configured with the same ring_name, in this graph or a
	 * sibling one sharing this --file-prefix) is a real
	 * misconfiguration, not something to silently paper over with a
	 * lookup fallback - graph_config.c's own init()-returns-NULL ==
	 * startup failure contract handles it the same way a bad path/
	 * config value anywhere else does. */
	st->ring = rte_ring_create(ring_name, ring_size, rte_socket_id(), 0);
	if (st->ring == NULL) {
		fprintf(stderr, "ring_output: rte_ring_create('%s') failed: %s\n",
			ring_name, rte_strerror(rte_errno));
		free(st);
		return NULL;
	}

	atomic_init(&st->next_seq, 0);
	atomic_init(&st->records_enqueued, 0);
	atomic_init(&st->records_dropped, 0);
	atomic_init(&st->bytes_enqueued, 0);
	return st;
}

struct stage_result
ring_output_stage_process(void *state, const struct stage_record *in, struct stage_record *out)
{
	struct ring_output_config *cfg = state;
	(void)out; /* a leaf - nothing downstream in this process's graph ever reads *out */

	size_t total = sizeof(struct chrono_record_hdr) + in->len;
	/* rte_malloc(), not malloc() - the whole point is that a
	 * DIFFERENT process (the downstream loomtabulator instance,
	 * attached as a DPDK secondary) dequeues and rte_free()'s this
	 * blob - see ring_input.h's own comment on why only DPDK's
	 * hugepage-backed allocator produces memory safely shared (or
	 * freed) across process boundaries at all. */
	uint8_t *blob = rte_malloc(NULL, total, 0);
	if (blob == NULL) {
		atomic_fetch_add_explicit(&cfg->records_dropped, 1, memory_order_relaxed);
		return (struct stage_result){ .ok = false, .drop_reason = "rte_malloc() failed" };
	}

	struct chrono_record_hdr *hdr = (struct chrono_record_hdr *)blob;
	hdr->magic = CHRONO_RECORD_MAGIC;
	hdr->seq = atomic_fetch_add_explicit(&cfg->next_seq, 1, memory_order_relaxed);
	hdr->capture_tsc = in->capture_tsc;
	hdr->len = in->len;
	hdr->reserved = 0;
	if (in->len > 0)
		memcpy(blob + sizeof(*hdr), in->data, in->len);

	if (rte_ring_enqueue(cfg->ring, blob) != 0) {
		rte_free(blob);
		atomic_fetch_add_explicit(&cfg->records_dropped, 1, memory_order_relaxed);
		return (struct stage_result){ .ok = false, .drop_reason = "output ring full" };
	}

	atomic_fetch_add_explicit(&cfg->records_enqueued, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&cfg->bytes_enqueued, in->len, memory_order_relaxed);
	return (struct stage_result){ .ok = true };
}

void
ring_output_stage_teardown(void *state)
{
	struct ring_output_config *cfg = state;
	if (cfg == NULL)
		return;

	/* Drain anything left unconsumed so its rte_malloc()'d blobs
	 * don't leak - same posture main.c's own shutdown path already
	 * takes for the input ring. Whether a downstream secondary is
	 * still attached and mid-dequeue when this runs is exactly the
	 * restart-ordering hazard docs/MANAGEMENT.md's Part 2 spec calls
	 * out - this stage can't do anything about that itself, it can
	 * only make sure ITS OWN shutdown doesn't leak memory. */
	void *leftover = NULL;
	while (rte_ring_dequeue(cfg->ring, &leftover) == 0)
		rte_free(leftover);

	free(cfg);
}

void
ring_output_stage_get_status(void *state, struct stage_status *out)
{
	struct ring_output_config *cfg = state;
	out->field_count = 3;
	snprintf(out->fields[0].name, STAGE_STATUS_NAME_MAX, "records_enqueued");
	out->fields[0].value = atomic_load_explicit(&cfg->records_enqueued, memory_order_relaxed);
	snprintf(out->fields[1].name, STAGE_STATUS_NAME_MAX, "records_dropped");
	out->fields[1].value = atomic_load_explicit(&cfg->records_dropped, memory_order_relaxed);
	snprintf(out->fields[2].name, STAGE_STATUS_NAME_MAX, "bytes_enqueued");
	out->fields[2].value = atomic_load_explicit(&cfg->bytes_enqueued, memory_order_relaxed);
}
