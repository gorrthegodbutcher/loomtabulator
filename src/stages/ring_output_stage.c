#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
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

	/* True only if THIS init() call is what created `ring` (rather than
	 * attaching to one that already existed) - see init()'s own comment
	 * for why this matters. Only the creator ever frees or drains it in
	 * teardown() below. */
	bool owns_ring;

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

	/* Tries to create first - the normal case for a real, freshly
	 * started instance. Falls back to rte_ring_lookup() ONLY on EEXIST,
	 * which is not always a misconfiguration: graph_config_load() is
	 * also run as a throwaway validate-then-teardown pass by
	 * POST /api/graph, POST /api/graphs/load, and POST /api/probe-port-
	 * count (see web_status.c) - every one of those calls this exact
	 * init() for real, including while the ACTUAL running pipeline
	 * already has its own ring_output node live on this same name. Without
	 * this fallback, simply re-saving (or even just Configure-probing) a
	 * graph containing an already-active ring_output node is impossible -
	 * the throwaway pass's own rte_ring_create() collides with the real,
	 * live one and fails every time. owns_ring tracks which case this
	 * was, since only the side that actually created the ring may ever
	 * free or drain it in teardown() below - a throwaway pass tearing
	 * down a ring it merely looked up must leave the real, live one
	 * completely untouched. This does mean two genuinely distinct
	 * ring_output nodes accidentally sharing one name no longer fails
	 * loudly at startup (they'll just both attach to the same ring,
	 * which rte_ring's own multi-producer safety makes harmless, if not
	 * necessarily intended) - accepted, since that's a much rarer
	 * mistake than "the graph I'm re-saving already has a live one." */
	st->ring = rte_ring_create(ring_name, ring_size, rte_socket_id(), 0);
	if (st->ring != NULL) {
		st->owns_ring = true;
	} else if (rte_errno == EEXIST) {
		st->ring = rte_ring_lookup(ring_name);
		st->owns_ring = false;
	}
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

	/* Only the instance that actually created this ring (see init()'s
	 * own comment) may drain or free it - a throwaway validation pass
	 * that merely attached to an already-live ring via rte_ring_lookup()
	 * must leave it completely alone: draining it here would steal real,
	 * in-flight records out from under the actual running pipeline, and
	 * freeing it would pull the ring out from under a downstream
	 * secondary that's still attached to it. */
	if (cfg->owns_ring) {
		/* Drain anything left unconsumed so its rte_malloc()'d blobs
		 * don't leak - same posture main.c's own shutdown path
		 * already takes for the input ring. Whether a downstream
		 * secondary is still attached and mid-dequeue when this runs
		 * is exactly the restart-ordering hazard docs/MANAGEMENT.md's
		 * Part 2 spec calls out - this stage can't do anything about
		 * that itself, it can only make sure ITS OWN shutdown doesn't
		 * leak memory. */
		void *leftover = NULL;
		while (rte_ring_dequeue(cfg->ring, &leftover) == 0)
			rte_free(leftover);

		/* Releases the ring's own name/memzone registration so a
		 * later instance reusing this ring_name (a restart of this
		 * same process, or a completely different graph) doesn't hit
		 * the EEXIST path in init() above unnecessarily. */
		rte_ring_free(cfg->ring);
	}

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

void
ring_output_stage_get_config_schema(struct stage_config_schema *out)
{
	out->field_count = 2;
	out->fields[0] = (struct stage_config_field){
		.name = "ring_name",
		.type = CONFIG_FIELD_STRING,
		.required = true,
		.description = "Name of the rte_ring to create - a downstream instance's own "
			       "input.ring_name must match this exactly.",
	};
	out->fields[1] = (struct stage_config_field){
		.name = "ring_size",
		.type = CONFIG_FIELD_INTEGER,
		.description = "Ring capacity in records (rounded up to a power of 2 by DPDK).",
		.has_min = true,
		.min = 1,
		.has_default = true,
		.default_value = "4096",
	};
}
