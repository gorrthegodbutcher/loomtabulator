#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rte_cycles.h>
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

void *
testgen_run(void *arg)
{
	struct testgen_config *cfg = arg;
	uint64_t sent = 0;
	uint64_t interval_us = cfg->rate_per_sec != 0 ? 1000000ULL / cfg->rate_per_sec : 0;

	while (!g_stop_requested && (cfg->count == 0 || sent < cfg->count)) {
		size_t total = sizeof(struct chrono_record_hdr) + cfg->payload_len;
		uint8_t *blob = malloc(total);
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
			free(blob); /* ring full - drop, same as any other
				      * backpressure-drop in this project family */

		sent++;
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
