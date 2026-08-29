#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <rte_cycles.h>
#include "pipeline_worker.h"
#include "record.h"

int
pipeline_worker_lcore_main(void *arg)
{
	struct pipeline_worker_ctx *ctx = arg;

	while (!*ctx->stop_requested) {
		epoch_barrier_wait_if_pending(ctx->barrier);

		void *item;
		if (rte_ring_dequeue_burst(ctx->ring, &item, 1, NULL) == 0) {
			rte_delay_us(200);
			continue;
		}

		struct chrono_record_hdr *hdr = item;
		if (hdr->magic == CHRONO_BARRIER_MAGIC) {
			uint64_t new_epoch = epoch_barrier_drain(ctx->barrier, hdr->seq, hdr->capture_tsc);
			fprintf(stderr, "loomtabulator: worker %u drained barrier -> epoch %" PRIu64
					 " (barrier seq=%" PRIu64 ")\n",
				ctx->worker_id, new_epoch, hdr->seq);
		} else {
			uint64_t seq = hdr->seq;
			uint64_t epoch = epoch_barrier_enter(ctx->barrier);
			pipeline_run(ctx->chain, &ctx->worker, ctx->counters, item,
				     (uint32_t)(sizeof(*hdr) + hdr->len), hdr->capture_tsc);
			epoch_barrier_exit(ctx->barrier);
			if (ctx->on_record_processed != NULL)
				ctx->on_record_processed(ctx->cb_arg, epoch, seq);
		}
		free(item);
	}

	return 0;
}
