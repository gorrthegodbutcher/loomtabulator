#ifndef RECORD_H
#define RECORD_H

/* Copied from chrontabulator's src/record.h - only the parts Loomtabulator
 * actually needs: the per-record wire/ring format, not chrontabulator's
 * own on-disk volume header/TOC/segment layout (this project never reads
 * or writes chrontabulator's device directly - Phase 4 receives records
 * over a shared rte_ring, already extracted from whatever segment they
 * came from). Keep in sync by hand if chrontabulator's record format ever
 * changes - see this project's own CLAUDE.md for the "vendor, don't
 * share a library" convention this whole project family uses.
 *
 * testgen.c builds records in exactly this shape so the synthetic input
 * used in v1 is byte-for-byte what a real chrontabulator replay (Phase 4)
 * will eventually put on the ring - see the plan's format-compatibility
 * verification step. */

#include <stdint.h>

#define CHRONO_MAGIC_U64(a, b, c, d, e, f, g, h) \
	((uint64_t)(uint8_t)(a) | ((uint64_t)(uint8_t)(b) << 8) | \
	 ((uint64_t)(uint8_t)(c) << 16) | ((uint64_t)(uint8_t)(d) << 24) | \
	 ((uint64_t)(uint8_t)(e) << 32) | ((uint64_t)(uint8_t)(f) << 40) | \
	 ((uint64_t)(uint8_t)(g) << 48) | ((uint64_t)(uint8_t)(h) << 56))

#define CHRONO_RECORD_MAGIC CHRONO_MAGIC_U64('C', 'H', 'R', 'R', 'E', 'C', '1', ' ')

/* Phase 2: a barrier record uses the exact same chrono_record_hdr framing
 * (so it flows through the same input ring, preserving the same overall
 * FIFO-across-consumers ordering a data record gets - see epoch_barrier.h)
 * but is never passed through the stage chain itself. len is always 0 (no
 * payload). capture_tsc carries the epoch boundary's timestamp. seq
 * carries the producer-assigned epoch id (monotonic, gapless, starting at
 * 0) - epoch_barrier_drain() cross-checks this against its own internally
 * tracked current_epoch at the moment the barrier is drained, catching
 * any drift between what the producer thinks the epoch boundary is and
 * what the consumer side thinks it is. */
#define CHRONO_BARRIER_MAGIC CHRONO_MAGIC_U64('C', 'H', 'R', 'B', 'A', 'R', '1', ' ')

/* One captured packet's worth of metadata, immediately followed by `len`
 * bytes of payload - identical layout to chrontabulator's own
 * chrono_record_hdr. seq is whatever the producer assigned it (in
 * chrontabulator, its own self-assigned, gapless per-segment counter);
 * Loomtabulator's validate stage checks magic and len sanity, nothing
 * else interprets seq's meaning. capture_tsc is an rte_rdtsc() reading
 * from whenever the record was originally captured - carried through
 * every pipeline stage unchanged (see stage.h's stage_record), and is
 * what Phase 2's epoch/watermark barrier will key off of. */
struct chrono_record_hdr {
	uint64_t magic;
	uint64_t seq;
	uint64_t capture_tsc;
	uint32_t len;
	uint32_t reserved;
};

#endif
